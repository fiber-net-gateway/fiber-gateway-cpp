#include "Http1ExchangeIo.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <limits>
#include <string>
#include <system_error>

#include "../common/Assert.h"
#include "Http1Connection.h"
#include "Http1HeaderParseBuffer.h"
#include "HttpExchange.h"
#include "HttpHeaderHash.h"
#include "HttpTransport.h"

namespace fiber::http {

namespace {

constexpr std::size_t kMaxDirectBodyRead = 64 * 1024;
constexpr std::size_t kInlineFirstWriteBodyLimit = 128;
constexpr std::size_t kHttpStatusDigits = 3;
constexpr std::size_t kMaxContentLengthDigits = 20;
constexpr std::size_t kMaxChunkSizeHexDigits = sizeof(std::size_t) * 2;
constexpr std::string_view kHttp10Prefix = "HTTP/1.0 ";
constexpr std::string_view kHttp11Prefix = "HTTP/1.1 ";
constexpr std::string_view kHeaderNameValueSep = ": ";
constexpr std::string_view kLineTerminator = "\r\n";
constexpr std::string_view kContentLengthHeader = "Content-Length: ";
constexpr std::string_view kChunkedHeader = "Transfer-Encoding: chunked\r\n";
constexpr std::string_view kConnectionCloseHeader = "Connection: close\r\n";
constexpr std::string_view kChunkedFinalPrefix = "0\r\n";
constexpr std::string_view kChunkedFinal = "0\r\n\r\n";

// Pre-hashed header names for response-header presence checks (per-response hot path).
constexpr std::uint64_t kContentLengthNameHash = http_header_name_hash("content-length");
constexpr std::uint64_t kTransferEncodingNameHash = http_header_name_hash("transfer-encoding");
constexpr std::uint64_t kConnectionNameHash = http_header_name_hash("connection");

std::string_view default_reason_phrase(int status) noexcept {
    switch (status) {
        case 100:
            return "Continue";
        case 101:
            return "Switching Protocols";
        case 200:
            return "OK";
        case 201:
            return "Created";
        case 202:
            return "Accepted";
        case 204:
            return "No Content";
        case 301:
            return "Moved Permanently";
        case 302:
            return "Found";
        case 304:
            return "Not Modified";
        case 400:
            return "Bad Request";
        case 401:
            return "Unauthorized";
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 408:
            return "Request Timeout";
        case 409:
            return "Conflict";
        case 413:
            return "Payload Too Large";
        case 429:
            return "Too Many Requests";
        case 500:
            return "Internal Server Error";
        case 501:
            return "Not Implemented";
        case 502:
            return "Bad Gateway";
        case 503:
            return "Service Unavailable";
        case 504:
            return "Gateway Timeout";
        default:
            return "OK";
    }
}

std::size_t append_decimal(char *dst, std::uint64_t value) noexcept {
    std::array<char, 32> buffer{};
    auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (ec != std::errc()) {
        return 0;
    }
    std::size_t len = static_cast<std::size_t>(ptr - buffer.data());
    std::memcpy(dst, buffer.data(), len);
    return len;
}

std::size_t append_hex(char *dst, std::size_t value) noexcept {
    std::array<char, 32> buffer{};
    auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, 16);
    if (ec != std::errc()) {
        return 0;
    }
    std::size_t len = static_cast<std::size_t>(ptr - buffer.data());
    std::memcpy(dst, buffer.data(), len);
    return len;
}

void append_bytes(char *&dst, std::string_view value) noexcept {
    std::memcpy(dst, value.data(), value.size());
    dst += value.size();
}

bool checked_add(std::size_t &total, std::size_t amount) noexcept {
    if (amount > std::numeric_limits<std::size_t>::max() - total) {
        return false;
    }
    total += amount;
    return true;
}

common::IoResult<void> append_encoded_header_fields_size(const HttpHeaders *headers, std::size_t &total) noexcept {
    if (headers == nullptr) {
        return common::IoResult<void>{};
    }

    for (auto it = headers->begin(); it != headers->end(); ++it) {
        const auto &field = *it;
        if (field.name_len == 0) {
            continue;
        }
        if (!checked_add(total, static_cast<std::size_t>(field.name_len)) ||
            !checked_add(total, kHeaderNameValueSep.size()) ||
            !checked_add(total, static_cast<std::size_t>(field.value_len)) ||
            !checked_add(total, kLineTerminator.size())) {
            return std::unexpected(common::IoErr::NoMem);
        }
    }
    return common::IoResult<void>{};
}

void encode_header_fields(char *&dst, const HttpHeaders *headers) noexcept {
    if (headers == nullptr) {
        return;
    }

    for (auto it = headers->begin(); it != headers->end(); ++it) {
        const auto &field = *it;
        if (field.name_len == 0) {
            continue;
        }
        append_bytes(dst, field.name_view());
        append_bytes(dst, kHeaderNameValueSep);
        append_bytes(dst, field.value_view());
        append_bytes(dst, kLineTerminator);
    }
}

fiber::async::Task<common::IoResult<void>> transport_write_all(HttpTransport *transport, const void *buf, size_t len,
                                                               std::chrono::milliseconds timeout) {
    const auto *ptr = static_cast<const std::uint8_t *>(buf);
    size_t remaining = len;
    while (remaining > 0) {
        auto result = co_await transport->write(ptr, remaining, timeout);
        if (!result) {
            co_return std::unexpected(result.error());
        }
        if (*result == 0) {
            co_return std::unexpected(common::IoErr::ConnReset);
        }
        ptr += *result;
        remaining -= *result;
    }
    co_return common::IoResult<void>{};
}

fiber::async::Task<common::IoResult<void>> transport_write_all(HttpTransport *transport, mem::IoBufChain &buf,
                                                               std::chrono::milliseconds timeout) {
    while (buf.readable_bytes() > 0) {
        auto result = co_await transport->writev(buf, timeout);
        if (!result) {
            co_return std::unexpected(result.error());
        }
        if (*result == 0) {
            co_return std::unexpected(common::IoErr::ConnReset);
        }
    }
    co_return common::IoResult<void>{};
}

Http1HeaderParseBufferOptions header_parse_buffer_options(const HttpServerOptions &options) noexcept {
    return Http1HeaderParseBufferOptions{
            .init_size = options.header_init_size,
            .large_size = options.header_large_size,
            .large_num = options.header_large_num,
    };
}

bool response_must_not_have_body(const HttpExchange &exchange, int status_code) noexcept {
    return exchange.method() == HttpMethod::Head || (status_code >= 100 && status_code < 200) || status_code == 204 ||
           status_code == 304;
}

} // namespace

Http1ExchangeIo::Http1ExchangeIo(Http1Connection &connection, const HttpExchange &exchange) : connection_(&connection) {
    const HttpBodySpec body_spec = exchange.request_body_spec_;
    if (body_spec.is_chunked()) {
        body_parser_.set_chunked();
        return;
    }
    if (!body_spec.is_content_length() || body_spec.content_length() == 0) {
        body_parser_.set_none();
        return;
    }
    body_parser_.set_content_length(body_spec.content_length());
}

Http1ExchangeIo::~Http1ExchangeIo() {
    FIBER_ASSERT(response_channel_closed_callback_ == nullptr);
    FIBER_ASSERT(!transport_terminal_callback_registered_);
}

bool Http1ExchangeIo::response_channel_closed() const noexcept {
    FIBER_ASSERT(connection_ != nullptr);
    return connection_->transport().terminal();
}

common::IoErr Http1ExchangeIo::set_response_channel_closed_callback(ResponseChannelClosedCallback callback,
                                                                    void *ctx) noexcept {
    if (callback == nullptr) {
        return common::IoErr::Invalid;
    }
    if (response_channel_closed_callback_ != nullptr) {
        return common::IoErr::Busy;
    }

    response_channel_closed_callback_ = callback;
    response_channel_closed_callback_ctx_ = ctx;
    transport_terminal_callback_registered_ = true;
    common::IoErr err = connection_->transport().set_terminal_callback(&Http1ExchangeIo::on_transport_terminal, this);
    if (err != common::IoErr::None) {
        FIBER_ASSERT(transport_terminal_callback_registered_);
        transport_terminal_callback_registered_ = false;
        response_channel_closed_callback_ = nullptr;
        response_channel_closed_callback_ctx_ = nullptr;
    }
    return err;
}

common::IoErr Http1ExchangeIo::clear_response_channel_closed_callback(ResponseChannelClosedCallback callback,
                                                                      void *ctx) noexcept {
    if (callback == nullptr) {
        return common::IoErr::Invalid;
    }
    if (response_channel_closed_callback_ != callback || response_channel_closed_callback_ctx_ != ctx) {
        return common::IoErr::None;
    }

    FIBER_ASSERT(transport_terminal_callback_registered_);
    common::IoErr err = connection_->transport().clear_terminal_callback(&Http1ExchangeIo::on_transport_terminal, this);
    if (err != common::IoErr::None) {
        return err;
    }
    transport_terminal_callback_registered_ = false;
    response_channel_closed_callback_ = nullptr;
    response_channel_closed_callback_ctx_ = nullptr;
    return common::IoErr::None;
}

void Http1ExchangeIo::on_transport_terminal(void *ctx, common::IoErr) noexcept {
    auto *io = static_cast<Http1ExchangeIo *>(ctx);
    FIBER_ASSERT(io != nullptr);
    FIBER_ASSERT(io->transport_terminal_callback_registered_);
    FIBER_ASSERT(io->response_channel_closed_callback_ != nullptr);

    ResponseChannelClosedCallback callback = io->response_channel_closed_callback_;
    void *callback_ctx = io->response_channel_closed_callback_ctx_;
    io->transport_terminal_callback_registered_ = false;
    io->response_channel_closed_callback_ = nullptr;
    io->response_channel_closed_callback_ctx_ = nullptr;
    callback(callback_ctx);
}

common::IoResult<void> Http1ExchangeIo::ensure_read_buf_writable(std::size_t min_writable) noexcept {
    if (min_writable == 0) {
        return {};
    }

    if (!read_buf_) {
        read_buf_ = mem::IoBuf::allocate(min_writable);
        if (!read_buf_) {
            return std::unexpected(common::IoErr::NoMem);
        }
        return {};
    }

    if (read_buf_.readable() == 0) {
        if (read_buf_.unique() && read_buf_.capacity() >= min_writable) {
            read_buf_.reset();
            return {};
        }

        mem::IoBuf next = mem::IoBuf::allocate(min_writable);
        if (!next) {
            return std::unexpected(common::IoErr::NoMem);
        }
        read_buf_ = std::move(next);
        return {};
    }

    if (read_buf_.writable() >= min_writable) {
        return {};
    }

    std::size_t unread = read_buf_.readable();
    mem::IoBuf next = mem::IoBuf::allocate(unread + min_writable);
    if (!next) {
        return std::unexpected(common::IoErr::NoMem);
    }
    std::memcpy(next.writable_data(), read_buf_.readable_data(), unread);
    next.commit(unread);
    read_buf_ = std::move(next);
    return {};
}

std::size_t Http1ExchangeIo::drain_body_input(mem::IoBuf &buffer) noexcept {
    std::size_t copied = 0;
    while (buffer.writable() > 0) {
        connection_->inbound_bufs().drop_empty_front();
        mem::IoBuf *front = connection_->inbound_bufs().front();
        if (front && front->readable() > 0) {
            std::size_t take = std::min(front->readable(), buffer.writable());
            std::memcpy(buffer.writable_data(), front->readable_data(), take);
            buffer.commit(take);
            connection_->inbound_bufs().consume_and_compact(take);
            copied += take;
            continue;
        }

        if (read_buf_.readable() == 0) {
            break;
        }

        std::size_t take = std::min(read_buf_.readable(), buffer.writable());
        std::memcpy(buffer.writable_data(), read_buf_.readable_data(), take);
        buffer.commit(take);
        read_buf_.consume(take);
        copied += take;
    }
    return copied;
}

std::size_t Http1ExchangeIo::body_input_readable() const noexcept {
    return connection_->inbound_bufs().readable_bytes() + read_buf_.readable();
}

mem::IoBuf *Http1ExchangeIo::front_body_input() noexcept {
    connection_->inbound_bufs().drop_empty_front();
    mem::IoBuf *front = connection_->inbound_bufs().front();
    if (front && front->readable() > 0) {
        return front;
    }
    if (read_buf_.readable() > 0) {
        return &read_buf_;
    }
    return nullptr;
}

common::IoResult<void> Http1ExchangeIo::spill_read_buf_to_inbound() noexcept {
    if (read_buf_.readable() == 0) {
        return {};
    }

    mem::IoBuf trailing = read_buf_.retain_slice(0, read_buf_.readable());
    if (!connection_->inbound_bufs().append(std::move(trailing))) {
        return std::unexpected(common::IoErr::NoMem);
    }
    read_buf_.consume(read_buf_.readable());
    return {};
}

fiber::async::Task<common::IoResult<size_t>> Http1ExchangeIo::read_more(std::size_t max_bytes,
                                                                        std::chrono::milliseconds timeout) noexcept {
    if (!connection_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    std::size_t read_size = std::min(max_bytes, kMaxDirectBodyRead);
    if (read_size == 0) {
        co_return static_cast<size_t>(0);
    }

    auto ensure_result = ensure_read_buf_writable(read_size);
    if (!ensure_result) {
        co_return std::unexpected(ensure_result.error());
    }

    auto result = co_await connection_->transport().read(read_buf_.writable_data(), read_size, timeout);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    read_call_used_io_ = true;
    read_buf_.commit(*result);
    co_return *result;
}

fiber::async::Task<common::IoResult<ParseCode>>
Http1ExchangeIo::advance_chunked_body(std::size_t max_bytes, bool allow_read,
                                      std::chrono::milliseconds timeout) noexcept {
    for (;;) {
        mem::IoBuf *front = front_body_input();
        if (!front || front->readable() == 0) {
            if (!allow_read) {
                co_return ParseCode::Again;
            }
            auto more = co_await read_more(max_bytes, timeout);
            if (!more) {
                co_return std::unexpected(more.error());
            }
            if (*more == 0) {
                co_return std::unexpected(common::IoErr::ConnReset);
            }
            allow_read = false;
            continue;
        }

        mem::IoBuf cursor(*front);
        ParseCode code = body_parser_.execute(&cursor);
        std::size_t consumed = front->readable() - cursor.readable();
        if (consumed > 0) {
            if (front == &read_buf_) {
                read_buf_.consume(consumed);
            } else {
                connection_->inbound_bufs().consume_and_compact(consumed);
            }
        }

        if (code == ParseCode::Again) {
            if (consumed == 0) {
                co_return std::unexpected(common::IoErr::Invalid);
            }
            continue;
        }
        if (code != ParseCode::Ok && code != ParseCode::Done && code != ParseCode::BodyDone) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
        co_return code;
    }
}

common::IoResult<void> Http1ExchangeIo::take_prefix(mem::IoBufChain &out, std::size_t len) noexcept {
    while (len > 0) {
        connection_->inbound_bufs().drop_empty_front();
        mem::IoBuf *front = connection_->inbound_bufs().front();
        if (front && front->readable() > 0) {
            std::size_t take = std::min(len, front->readable());
            if (!connection_->inbound_bufs().take_prefix(take, out)) {
                return std::unexpected(common::IoErr::NoMem);
            }
            len -= take;
            continue;
        }

        if (read_buf_.readable() == 0) {
            return std::unexpected(common::IoErr::Invalid);
        }

        std::size_t take = std::min(len, read_buf_.readable());
        mem::IoBuf piece = read_buf_.retain_slice(0, take);
        if (!out.append(std::move(piece))) {
            return std::unexpected(common::IoErr::NoMem);
        }
        read_buf_.consume(take);
        len -= take;
    }
    return {};
}

fiber::async::Task<common::IoResult<mem::IoBufChain>>
Http1ExchangeIo::read_body(HttpExchange &exchange, size_t max_bytes, std::chrono::milliseconds timeout) noexcept {
    if (!connection_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    mem::IoBufChain out(connection_->loop().io_buf_node_pool());
    if (raw_stream_active()) {
        if (max_bytes == 0) {
            co_return out;
        }
        if (body_input_readable() == 0) {
            auto more = co_await read_more(max_bytes, timeout);
            if (!more) {
                co_return std::unexpected(more.error());
            }
            if (*more == 0) {
                out.mark_complete();
                co_return out;
            }
        }

        const std::size_t take = std::min(max_bytes, body_input_readable());
        auto take_result = take_prefix(out, take);
        if (!take_result) {
            co_return std::unexpected(take_result.error());
        }
        co_return out;
    }

    if (body_parser_.done()) {
        exchange.request_trailers_complete_ = true;
        out.mark_complete();
        co_return out;
    }

    if (body_parser_.type() == BodyParser::Type::Chunked) {
        if (max_bytes == 0) {
            co_return out;
        }

        std::size_t remaining_budget = max_bytes;
        read_call_used_io_ = false;
        for (;;) {
            if (body_parser_.remaining() == 0) {
                auto parse_result = co_await advance_chunked_body(remaining_budget, !read_call_used_io_, timeout);
                if (!parse_result) {
                    co_return std::unexpected(parse_result.error());
                }
                if (*parse_result == ParseCode::BodyDone) {
                    auto trailer_result = co_await read_request_trailers(exchange, timeout);
                    if (!trailer_result) {
                        co_return std::unexpected(trailer_result.error());
                    }
                    out.mark_complete();
                    co_return out;
                }
                if (*parse_result == ParseCode::Again) {
                    co_return out;
                }
            }

            if (body_input_readable() == 0) {
                if (read_call_used_io_) {
                    co_return out;
                }
                auto more = co_await read_more(std::min(remaining_budget, body_parser_.remaining()), timeout);
                if (!more) {
                    co_return std::unexpected(more.error());
                }
                if (*more == 0) {
                    co_return std::unexpected(common::IoErr::ConnReset);
                }
            }

            std::size_t take = std::min({remaining_budget, body_parser_.remaining(), body_input_readable()});
            auto take_result = take_prefix(out, take);
            if (!take_result) {
                co_return std::unexpected(take_result.error());
            }
            body_parser_.consume(take);
            remaining_budget -= take;

            if (remaining_budget == 0 || body_parser_.remaining() > 0) {
                co_return out;
            }
        }
    }

    if (max_bytes == 0) {
        co_return out;
    }

    read_call_used_io_ = false;
    if (body_input_readable() == 0) {
        auto more = co_await read_more(std::min(max_bytes, body_parser_.remaining()), timeout);
        if (!more) {
            co_return std::unexpected(more.error());
        }
        if (*more == 0) {
            co_return std::unexpected(common::IoErr::ConnReset);
        }
    }

    std::size_t take = std::min({max_bytes, body_parser_.remaining(), body_input_readable()});
    auto take_result = take_prefix(out, take);
    if (!take_result) {
        co_return std::unexpected(take_result.error());
    }
    body_parser_.consume(take);
    if (body_parser_.done()) {
        exchange.request_trailers_complete_ = true;
        out.mark_complete();
    }
    co_return out;
}

fiber::async::Task<common::IoResult<void>>
Http1ExchangeIo::read_request_trailers(HttpExchange &exchange, std::chrono::milliseconds timeout) noexcept {
    if (!connection_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    Http1HeaderParseBuffer header_buffer(header_parse_buffer_options(connection_->options()));
    auto init_result = header_buffer.ensure_init();
    if (!init_result) {
        co_return std::unexpected(init_result.error());
    }

    HeaderLineParser parser(connection_->options());
    for (;;) {
        ParseCode code = parser.execute(&header_buffer.buf());
        if (code == ParseCode::Again) {
            if (header_buffer.buf().writable() == 0) {
                if (!header_buffer.can_grow()) {
                    co_return std::unexpected(common::IoErr::Invalid);
                }
                auto grow_result = header_buffer.grow_and_replace(parser);
                if (!grow_result) {
                    co_return std::unexpected(grow_result.error());
                }
            }
            std::size_t copied = drain_body_input(header_buffer.buf());
            if (copied == 0) {
                auto more = co_await connection_->transport().read_into(header_buffer.buf(), timeout);
                if (!more) {
                    co_return std::unexpected(more.error());
                }
                if (*more == 0) {
                    co_return std::unexpected(common::IoErr::ConnReset);
                }
            }
            continue;
        }

        if (code == ParseCode::Ok) {
            const auto &line = parser.state();
            if (!line.header_name_start || !line.header_name_end || line.header_name_end < line.header_name_start) {
                co_return std::unexpected(common::IoErr::Invalid);
            }

            std::size_t name_len = static_cast<std::size_t>(line.header_name_end - line.header_name_start);
            std::string_view name(reinterpret_cast<char *>(line.header_name_start), name_len);
            std::string_view value;
            if (line.header_start && line.header_end && line.header_end >= line.header_start) {
                std::size_t value_len = static_cast<std::size_t>(line.header_end - line.header_start);
                value = std::string_view(reinterpret_cast<char *>(line.header_start), value_len);
            }

            const char *lowcase_name = nullptr;
            if (line.lowcase_index == name_len) {
                lowcase_name = reinterpret_cast<const char *>(line.lowcase_header);
            }
            HttpHeaders::HeaderField *field =
                    lowcase_name ? exchange.request_trailers_.add(name, value, lowcase_name, line.header_hash)
                                 : exchange.request_trailers_.add_prehashed(name, value, line.header_hash);
            if (!field) {
                co_return std::unexpected(common::IoErr::NoMem);
            }
            continue;
        }

        if (code == ParseCode::HeaderDone) {
            if (header_buffer.buf().readable() > 0) {
                auto trailing_result = header_buffer.retain_suffix();
                if (!trailing_result) {
                    co_return std::unexpected(trailing_result.error());
                }
                mem::IoBuf trailing = std::move(*trailing_result);
                if (!connection_->inbound_bufs().append(std::move(trailing))) {
                    co_return std::unexpected(common::IoErr::NoMem);
                }
            }
            auto spill_result = spill_read_buf_to_inbound();
            if (!spill_result) {
                co_return std::unexpected(spill_result.error());
            }
            exchange.request_trailers_complete_ = true;
            body_parser_.finish_chunked_trailers();
            co_return common::IoResult<void>{};
        }

        co_return std::unexpected(common::IoErr::Invalid);
    }
}

common::IoErr Http1ExchangeIo::prepare_final_header(const HttpExchange &exchange,
                                                    const OutgoingHeaderBlockView &header) noexcept {
    if (response_phase_ != ResponsePhase::Init) {
        return common::IoErr::Already;
    }
    if (header.kind != OutgoingHeaderKind::Final) {
        return common::IoErr::Invalid;
    }
    HttpBodySpec body_spec = header.body;
    const bool raw_stream = body_spec.is_stream();
    if ((header.status_code < 200 && !(header.status_code == 101 && raw_stream)) || header.status_code > 999) {
        return common::IoErr::Invalid;
    }
    if (raw_stream && header.end_stream) {
        return common::IoErr::Invalid;
    }
    const bool must_not_have_body = !raw_stream && response_must_not_have_body(exchange, header.status_code);
    if (body_spec.is_none() && !must_not_have_body) {
        return common::IoErr::Invalid;
    }
    if (must_not_have_body) {
        if (body_spec.is_chunked()) {
            return common::IoErr::Invalid;
        }
        if (body_spec.is_content_length() && body_spec.content_length() != 0) {
            return common::IoErr::Invalid;
        }
    }
    if (header.end_stream) {
        if (body_spec.is_content_length() && body_spec.content_length() != 0) {
            return common::IoErr::Invalid;
        }
        if (body_spec.is_chunked()) {
            body_spec = HttpBodySpec::Auto();
        }
    }
    if (!header.end_stream && body_spec.is_none()) {
        return common::IoErr::Invalid;
    }

    response_status_code_ = header.status_code;
    response_reason_ = header.reason;
    response_headers_ = header.headers;
    response_body_spec_ = body_spec;
    response_connection_mode_ = header.connection_mode;
    response_content_length_ = body_spec.is_content_length() ? body_spec.content_length() : 0;

    if (exchange.version_ == HttpVersion::HTTP_2_0 && response_connection_mode_ == ResponseConnectionMode::Close) {
        return common::IoErr::NotSupported;
    }
    return common::IoErr::None;
}

common::IoResult<void> Http1ExchangeIo::normalize_response_plan(bool body_end, std::size_t first_body_len,
                                                                bool infer_body_mode,
                                                                HttpBodySpec &body_spec) const noexcept {
    body_spec = response_body_spec_;

    if (infer_body_mode && body_spec.is_auto()) {
        if (body_end) {
            body_spec = HttpBodySpec::ContentLength(first_body_len);
        } else {
            body_spec = HttpBodySpec::Chunked();
        }
    }

    if (body_spec.is_auto()) {
        body_spec = body_end ? HttpBodySpec::ContentLength(first_body_len) : HttpBodySpec::Chunked();
    }

    if (body_end) {
        if (body_spec.is_none()) {
            if (first_body_len != 0) {
                return std::unexpected(common::IoErr::Invalid);
            }
            return common::IoResult<void>{};
        }
        if (body_spec.is_content_length() && first_body_len != body_spec.content_length()) {
            return std::unexpected(common::IoErr::Invalid);
        }
    } else if (body_spec.is_none()) {
        return std::unexpected(common::IoErr::Invalid);
    }

    return common::IoResult<void>{};
}

bool Http1ExchangeIo::compute_close_conn(const HttpExchange &exchange) const noexcept {
    bool close_conn = response_connection_mode_ == ResponseConnectionMode::Close || connection_->stopping();
    if (!close_conn) {
        if (!connection_->options().drain_unread_body && !body_parser_.done()) {
            close_conn = true;
        } else if (exchange.version_ == HttpVersion::HTTP_1_0) {
            close_conn = !exchange.request_keep_alive_;
        } else {
            close_conn = exchange.request_close_;
        }
    }
    return close_conn;
}

common::IoResult<mem::IoBuf> Http1ExchangeIo::build_response_header(HttpExchange &exchange, bool body_end,
                                                                    std::size_t first_body_len, bool infer_body_mode,
                                                                    HttpBodySpec &body_spec,
                                                                    bool &close_conn) noexcept {
    auto normalize_result = normalize_response_plan(body_end, first_body_len, infer_body_mode, body_spec);
    if (!normalize_result) {
        return std::unexpected(normalize_result.error());
    }

    close_conn = compute_close_conn(exchange);

    std::string_view reason = response_reason_.empty() ? default_reason_phrase(response_status_code_)
                                                       : std::string_view(response_reason_);
    std::string_view version = exchange.version_ == HttpVersion::HTTP_1_0 ? kHttp10Prefix : kHttp11Prefix;

    bool write_content_length =
            body_spec.is_content_length() &&
            (response_headers_ == nullptr || !response_headers_->contains("content-length", kContentLengthNameHash));
    bool write_transfer_encoding =
            body_spec.is_chunked() && (response_headers_ == nullptr ||
                                       !response_headers_->contains("transfer-encoding", kTransferEncodingNameHash));
    bool write_connection_close =
            !body_spec.is_stream() && close_conn &&
            (response_headers_ == nullptr || !response_headers_->contains("connection", kConnectionNameHash));

    std::size_t header_len = 0;
    if (!checked_add(header_len, version.size()) || !checked_add(header_len, kHttpStatusDigits) ||
        !checked_add(header_len, 1) || !checked_add(header_len, reason.size()) ||
        !checked_add(header_len, kLineTerminator.size())) {
        return std::unexpected(common::IoErr::NoMem);
    }
    if (write_content_length) {
        if (!checked_add(header_len, kContentLengthHeader.size()) ||
            !checked_add(header_len, kMaxContentLengthDigits) || !checked_add(header_len, kLineTerminator.size())) {
            return std::unexpected(common::IoErr::NoMem);
        }
    }
    if (write_transfer_encoding) {
        if (!checked_add(header_len, kChunkedHeader.size())) {
            return std::unexpected(common::IoErr::NoMem);
        }
    }
    if (write_connection_close) {
        if (!checked_add(header_len, kConnectionCloseHeader.size())) {
            return std::unexpected(common::IoErr::NoMem);
        }
    }
    auto fields_size_result = append_encoded_header_fields_size(response_headers_, header_len);
    if (!fields_size_result) {
        return std::unexpected(fields_size_result.error());
    }
    if (!checked_add(header_len, kLineTerminator.size())) {
        return std::unexpected(common::IoErr::NoMem);
    }

    mem::IoBuf header = mem::IoBuf::allocate(header_len);
    if (!header) {
        return std::unexpected(common::IoErr::NoMem);
    }

    char *out = reinterpret_cast<char *>(header.writable_data());
    char *begin = out;
    append_bytes(out, version);
    std::size_t status_len = append_decimal(out, static_cast<std::uint64_t>(response_status_code_));
    if (status_len == 0) {
        return std::unexpected(common::IoErr::Invalid);
    }
    out += status_len;
    *out++ = ' ';
    append_bytes(out, reason);
    append_bytes(out, kLineTerminator);

    if (write_content_length) {
        append_bytes(out, kContentLengthHeader);
        std::size_t value_len = append_decimal(out, body_spec.content_length());
        if (value_len == 0) {
            return std::unexpected(common::IoErr::Invalid);
        }
        out += value_len;
        append_bytes(out, kLineTerminator);
    }
    if (write_transfer_encoding) {
        append_bytes(out, kChunkedHeader);
    }
    if (write_connection_close) {
        append_bytes(out, kConnectionCloseHeader);
    }

    encode_header_fields(out, response_headers_);
    append_bytes(out, kLineTerminator);
    header.commit(static_cast<std::size_t>(out - begin));

    return header;
}

common::IoResult<mem::IoBuf> Http1ExchangeIo::build_informational_header(const HttpExchange &exchange, int status_code,
                                                                         const HttpHeaders *headers) const noexcept {
    std::string_view reason = default_reason_phrase(status_code);
    std::string_view version = exchange.version_ == HttpVersion::HTTP_1_0 ? kHttp10Prefix : kHttp11Prefix;

    std::size_t header_len = 0;
    if (!checked_add(header_len, version.size()) || !checked_add(header_len, kHttpStatusDigits) ||
        !checked_add(header_len, 1) || !checked_add(header_len, reason.size()) ||
        !checked_add(header_len, kLineTerminator.size())) {
        return std::unexpected(common::IoErr::NoMem);
    }
    auto fields_size_result = append_encoded_header_fields_size(headers, header_len);
    if (!fields_size_result) {
        return std::unexpected(fields_size_result.error());
    }
    if (!checked_add(header_len, kLineTerminator.size())) {
        return std::unexpected(common::IoErr::NoMem);
    }

    mem::IoBuf header = mem::IoBuf::allocate(header_len);
    if (!header) {
        return std::unexpected(common::IoErr::NoMem);
    }

    char *out = reinterpret_cast<char *>(header.writable_data());
    char *begin = out;
    append_bytes(out, version);
    std::size_t status_len = append_decimal(out, static_cast<std::uint64_t>(status_code));
    if (status_len == 0) {
        return std::unexpected(common::IoErr::Invalid);
    }
    out += status_len;
    *out++ = ' ';
    append_bytes(out, reason);
    append_bytes(out, kLineTerminator);
    encode_header_fields(out, headers);
    append_bytes(out, kLineTerminator);
    header.commit(static_cast<std::size_t>(out - begin));
    return header;
}

common::IoResult<mem::IoBuf> Http1ExchangeIo::build_chunked_trailer_block(const HttpHeaders *headers,
                                                                          bool include_final_chunk) const noexcept {
    std::size_t total = 0;
    if (include_final_chunk && !checked_add(total, kChunkedFinalPrefix.size())) {
        return std::unexpected(common::IoErr::NoMem);
    }
    auto fields_size_result = append_encoded_header_fields_size(headers, total);
    if (!fields_size_result) {
        return std::unexpected(fields_size_result.error());
    }
    if (!checked_add(total, kLineTerminator.size())) {
        return std::unexpected(common::IoErr::NoMem);
    }

    mem::IoBuf block = mem::IoBuf::allocate(total);
    if (!block) {
        return std::unexpected(common::IoErr::NoMem);
    }

    char *out = reinterpret_cast<char *>(block.writable_data());
    char *begin = out;
    if (include_final_chunk) {
        append_bytes(out, kChunkedFinalPrefix);
    }
    encode_header_fields(out, headers);
    append_bytes(out, kLineTerminator);
    block.commit(static_cast<std::size_t>(out - begin));
    return block;
}

fiber::async::Task<common::IoResult<void>>
Http1ExchangeIo::write_response_header(HttpExchange &exchange, bool body_end, std::size_t first_body_len,
                                       bool infer_body_mode, std::chrono::milliseconds timeout) noexcept {
    if (!connection_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (response_phase_ != ResponsePhase::Init) {
        co_return std::unexpected(common::IoErr::Already);
    }

    HttpBodySpec body_spec = HttpBodySpec::Auto();
    bool close_conn = false;
    auto header_result =
            build_response_header(exchange, body_end, first_body_len, infer_body_mode, body_spec, close_conn);
    if (!header_result) {
        co_return std::unexpected(header_result.error());
    }

    auto result = co_await transport_write_all(&connection_->transport(), header_result->readable_data(),
                                               header_result->readable(), timeout);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    response_body_spec_ = body_spec;
    response_content_length_ = body_spec.is_content_length() ? body_spec.content_length() : 0;
    response_phase_ = ResponsePhase::HeaderSent;
    close_after_response_ = close_conn;
    if (body_end) {
        response_phase_ = ResponsePhase::Finished;
    }
    co_return common::IoResult<void>{};
}

fiber::async::Task<common::IoResult<void>>
Http1ExchangeIo::write_informational_header(HttpExchange &exchange, int status_code, const HttpHeaders *headers,
                                            std::chrono::milliseconds timeout) noexcept {
    if (!connection_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (response_phase_ != ResponsePhase::Init) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (status_code < 100 || status_code >= 200) {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    auto header_result = build_informational_header(exchange, status_code, headers);
    if (!header_result) {
        co_return std::unexpected(header_result.error());
    }

    auto result = co_await transport_write_all(&connection_->transport(), header_result->readable_data(),
                                               header_result->readable(), timeout);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    co_return common::IoResult<void>{};
}

fiber::async::Task<common::IoResult<void>> Http1ExchangeIo::send_header(HttpExchange &exchange,
                                                                        const OutgoingHeaderBlockView &header,
                                                                        std::chrono::milliseconds timeout) {
    switch (header.kind) {
        case OutgoingHeaderKind::Informational:
            if (header.end_stream) {
                co_return std::unexpected(common::IoErr::Invalid);
            }
            co_return co_await write_informational_header(exchange, header.status_code, header.headers, timeout);
        case OutgoingHeaderKind::Final: {
            common::IoErr err = prepare_final_header(exchange, header);
            if (err != common::IoErr::None) {
                co_return std::unexpected(err);
            }
            co_return co_await write_response_header(exchange, header.end_stream, 0, false, timeout);
        }
        case OutgoingHeaderKind::Trailer:
            if (response_phase_ == ResponsePhase::Init || response_phase_ == ResponsePhase::Finished) {
                co_return std::unexpected(common::IoErr::Invalid);
            }
            if (!response_body_spec_.is_chunked() || !header.end_stream) {
                co_return std::unexpected(common::IoErr::Invalid);
            }
            auto trailer_result = co_await write_chunked_trailer_block(header.headers, timeout);
            if (!trailer_result) {
                co_return std::unexpected(trailer_result.error());
            }
            response_phase_ = ResponsePhase::Finished;
            co_return common::IoResult<void>{};
    }

    co_return std::unexpected(common::IoErr::Invalid);
}

fiber::async::Task<common::IoResult<void>>
Http1ExchangeIo::write_chunked_trailer_block(const HttpHeaders *headers, std::chrono::milliseconds timeout) noexcept {
    auto block_result = build_chunked_trailer_block(headers, true);
    if (!block_result) {
        co_return std::unexpected(block_result.error());
    }

    auto result = co_await transport_write_all(&connection_->transport(), block_result->readable_data(),
                                               block_result->readable(), timeout);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    co_return common::IoResult<void>{};
}

fiber::async::Task<common::IoResult<void>>
Http1ExchangeIo::write_chunk_suffix(bool end, std::chrono::milliseconds timeout) noexcept {
    const std::string_view suffix = end ? std::string_view("\r\n0\r\n\r\n", 7) : std::string_view("\r\n", 2);
    co_return co_await transport_write_all(&connection_->transport(), suffix.data(), suffix.size(), timeout);
}

fiber::async::Task<common::IoResult<size_t>> Http1ExchangeIo::write_all(HttpExchange &exchange, mem::IoBufChain chunk,
                                                                        std::chrono::milliseconds timeout) noexcept {
    if (!connection_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (response_phase_ == ResponsePhase::Finished) {
        co_return std::unexpected(common::IoErr::Already);
    }

    size_t len = chunk.readable_bytes();
    if (chunk_write_active_) {
        const bool end = chunk.complete();
        if (len != chunk_payload_remaining_ || end != chunk_write_end_) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
        std::size_t total = 0;
        while (chunk_write_active_ || chunk.readable_bytes() != 0 || chunk.complete()) {
            auto written = co_await write(exchange, chunk, timeout);
            if (!written) {
                co_return std::unexpected(written.error());
            }
            total += *written;
        }
        co_return total;
    }
    if (response_phase_ == ResponsePhase::Init) {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    if (response_body_spec_.is_stream()) {
        if (len > 0) {
            auto res = co_await transport_write_all(&connection_->transport(), chunk, timeout);
            if (!res) {
                co_return std::unexpected(res.error());
            }
        }
        response_body_sent_ += len;
        if (len > 0) {
            response_phase_ = ResponsePhase::BodyStreaming;
        }
        if (chunk.complete()) {
            response_phase_ = ResponsePhase::Finished;
        }
        co_return len;
    }

    if (response_body_spec_.is_chunked()) {
        if (len > 0) {
            // Build [prefix][body][suffix] as one chain and issue a single writev.
            // Prefix: hex(len) + CRLF. Suffix: trailing CRLF, folded into the final
            // terminator when this is the last chunk ("\r\n0\r\n\r\n") so it is one node.
            std::array<char, kMaxChunkSizeHexDigits + 2> prefix_buf{};
            char *prefix_ptr = prefix_buf.data();
            prefix_ptr += append_hex(prefix_ptr, len);
            *prefix_ptr++ = '\r';
            *prefix_ptr++ = '\n';
            std::size_t prefix_len = static_cast<std::size_t>(prefix_ptr - prefix_buf.data());

            mem::IoBuf prefix = mem::IoBuf::allocate(prefix_len);
            if (!prefix) {
                co_return std::unexpected(common::IoErr::NoMem);
            }
            std::memcpy(prefix.writable_data(), prefix_buf.data(), prefix_len);
            prefix.commit(prefix_len);
            if (!chunk.prepend(std::move(prefix))) {
                co_return std::unexpected(common::IoErr::NoMem);
            }

            bool last = chunk.complete();
            std::string_view suffix = last ? std::string_view("\r\n0\r\n\r\n", 7) : std::string_view("\r\n", 2);
            mem::IoBuf suffix_buf = mem::IoBuf::allocate(suffix.size());
            if (!suffix_buf) {
                co_return std::unexpected(common::IoErr::NoMem);
            }
            std::memcpy(suffix_buf.writable_data(), suffix.data(), suffix.size());
            suffix_buf.commit(suffix.size());
            if (!chunk.append(std::move(suffix_buf))) {
                co_return std::unexpected(common::IoErr::NoMem);
            }

            auto res = co_await transport_write_all(&connection_->transport(), chunk, timeout);
            if (!res) {
                co_return std::unexpected(res.error());
            }
            response_body_sent_ += len;
            if (len > 0) {
                response_phase_ = ResponsePhase::BodyStreaming;
            }
            if (last) {
                response_phase_ = ResponsePhase::Finished;
            }
        } else if (chunk.complete()) {
            // Empty final chunk: just the terminator.
            auto zero_result = co_await transport_write_all(&connection_->transport(), "0\r\n\r\n", 5, timeout);
            if (!zero_result) {
                co_return std::unexpected(zero_result.error());
            }
            response_phase_ = ResponsePhase::Finished;
        }
        co_return len;
    }

    if (response_body_spec_.is_none()) {
        if (len != 0 || chunk.complete()) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
        co_return len;
    }

    if (response_body_spec_.is_content_length()) {
        size_t remaining = response_content_length_ - response_body_sent_;
        if (len > remaining) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
    }
    if (len > 0) {
        auto res = co_await transport_write_all(&connection_->transport(), chunk, timeout);
        if (!res) {
            co_return std::unexpected(res.error());
        }
    }
    response_body_sent_ += len;
    if (len > 0) {
        response_phase_ = ResponsePhase::BodyStreaming;
    }
    if (chunk.complete()) {
        if (response_body_spec_.is_content_length() && response_body_sent_ != response_content_length_) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
        response_phase_ = ResponsePhase::Finished;
    }
    co_return len;
}

fiber::async::Task<common::IoResult<size_t>> Http1ExchangeIo::write_all(HttpExchange &exchange, const uint8_t *buf,
                                                                        size_t len, bool end,
                                                                        std::chrono::milliseconds timeout) noexcept {
    if (len > 0 && buf == nullptr) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (!connection_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (response_phase_ == ResponsePhase::Finished) {
        co_return std::unexpected(common::IoErr::Already);
    }

    if (chunk_write_active_) {
        if (len != chunk_payload_remaining_ || end != chunk_write_end_) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
        const std::uint8_t *current = buf;
        std::size_t remaining = len;
        while (chunk_write_active_ || remaining != 0 || end) {
            auto written = co_await write(exchange, current, remaining, end, timeout);
            if (!written) {
                co_return std::unexpected(written.error());
            }
            current += *written;
            remaining -= *written;
            if (remaining == 0) {
                break;
            }
        }
        co_return len;
    }

    if (response_phase_ == ResponsePhase::Init) {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    if (response_body_spec_.is_stream()) {
        if (len > 0) {
            auto res = co_await transport_write_all(&connection_->transport(), buf, len, timeout);
            if (!res) {
                co_return std::unexpected(res.error());
            }
        }
        response_body_sent_ += len;
        if (len > 0) {
            response_phase_ = ResponsePhase::BodyStreaming;
        }
        if (end) {
            response_phase_ = ResponsePhase::Finished;
        }
        co_return len;
    }

    if (response_body_spec_.is_chunked()) {
        if (len > 0) {
            std::array<char, 32> size_buf{};
            auto [ptr, ec] = std::to_chars(size_buf.data(), size_buf.data() + size_buf.size(), len, 16);
            if (ec != std::errc()) {
                co_return std::unexpected(common::IoErr::Invalid);
            }
            std::string_view size_view(size_buf.data(), static_cast<size_t>(ptr - size_buf.data()));
            auto res = co_await transport_write_all(&connection_->transport(), size_view.data(), size_view.size(),
                                                    timeout);
            if (!res) {
                co_return std::unexpected(res.error());
            }
            res = co_await transport_write_all(&connection_->transport(), "\r\n", 2, timeout);
            if (!res) {
                co_return std::unexpected(res.error());
            }
            res = co_await transport_write_all(&connection_->transport(), buf, len, timeout);
            if (!res) {
                co_return std::unexpected(res.error());
            }
            res = co_await transport_write_all(&connection_->transport(), "\r\n", 2, timeout);
            if (!res) {
                co_return std::unexpected(res.error());
            }
        }
        response_body_sent_ += len;
        if (len > 0) {
            response_phase_ = ResponsePhase::BodyStreaming;
        }
        if (end) {
            auto zero_result = co_await transport_write_all(&connection_->transport(), "0\r\n\r\n", 5, timeout);
            if (!zero_result) {
                co_return std::unexpected(zero_result.error());
            }
            response_phase_ = ResponsePhase::Finished;
        }
        co_return len;
    }

    if (response_body_spec_.is_none()) {
        if (len != 0 || end) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
        co_return len;
    }

    if (response_body_spec_.is_content_length()) {
        size_t remaining = response_content_length_ - response_body_sent_;
        if (len > remaining) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
    }
    if (len > 0) {
        auto res = co_await transport_write_all(&connection_->transport(), buf, len, timeout);
        if (!res) {
            co_return std::unexpected(res.error());
        }
    }
    response_body_sent_ += len;
    if (len > 0) {
        response_phase_ = ResponsePhase::BodyStreaming;
    }
    if (end) {
        if (response_body_spec_.is_content_length() && response_body_sent_ != response_content_length_) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
        response_phase_ = ResponsePhase::Finished;
    }
    co_return len;
}

fiber::async::Task<common::IoResult<size_t>> Http1ExchangeIo::write(HttpExchange &exchange, mem::IoBufChain &chunk,
                                                                    std::chrono::milliseconds timeout) noexcept {
    if (!connection_ || exchange.io_ != this || response_phase_ == ResponsePhase::Init) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (response_phase_ == ResponsePhase::Finished) {
        co_return std::unexpected(common::IoErr::Already);
    }

    const std::size_t len = chunk.readable_bytes();
    const bool end = chunk.complete();

    if (response_body_spec_.is_none()) {
        if (len != 0 || end) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
        co_return 0;
    }

    if (response_body_spec_.is_content_length()) {
        if (response_body_sent_ > response_content_length_) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
        const std::size_t remaining = response_content_length_ - response_body_sent_;
        if (len > remaining || (end && len != remaining)) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
    }

    if (!response_body_spec_.is_chunked()) {
        if (len == 0) {
            if (end) {
                chunk.clear_complete();
                response_phase_ = ResponsePhase::Finished;
            }
            co_return 0;
        }
        auto written = co_await connection_->transport().writev(chunk, timeout);
        if (!written) {
            co_return std::unexpected(written.error());
        }
        if (*written == 0) {
            co_return std::unexpected(common::IoErr::ConnReset);
        }
        response_body_sent_ += *written;
        response_phase_ = ResponsePhase::BodyStreaming;
        if (*written == len && end) {
            chunk.clear_complete();
            response_phase_ = ResponsePhase::Finished;
        }
        co_return *written;
    }

    if (chunk_write_active_) {
        if (len != chunk_payload_remaining_ || end != chunk_write_end_) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
        if (chunk_suffix_pending_) {
            auto suffix = co_await write_chunk_suffix(end, timeout);
            if (!suffix) {
                co_return std::unexpected(suffix.error());
            }
            chunk_write_active_ = false;
            chunk_write_end_ = false;
            chunk_suffix_pending_ = false;
            if (end) {
                chunk.clear_complete();
                response_phase_ = ResponsePhase::Finished;
            }
            co_return 0;
        }
    } else {
        if (len == 0) {
            if (!end) {
                co_return 0;
            }
            auto finished = co_await transport_write_all(&connection_->transport(), "0\r\n\r\n", 5, timeout);
            if (!finished) {
                co_return std::unexpected(finished.error());
            }
            chunk.clear_complete();
            response_phase_ = ResponsePhase::Finished;
            co_return 0;
        }

        std::array<char, kMaxChunkSizeHexDigits + 2> prefix_buf{};
        char *prefix_ptr = prefix_buf.data();
        prefix_ptr += append_hex(prefix_ptr, len);
        *prefix_ptr++ = '\r';
        *prefix_ptr++ = '\n';
        const std::size_t prefix_len = static_cast<std::size_t>(prefix_ptr - prefix_buf.data());
        mem::IoBuf prefix = mem::IoBuf::allocate(prefix_len);
        if (!prefix) {
            co_return std::unexpected(common::IoErr::NoMem);
        }
        std::memcpy(prefix.writable_data(), prefix_buf.data(), prefix_len);
        prefix.commit(prefix_len);
        if (!chunk.prepend(std::move(prefix))) {
            co_return std::unexpected(common::IoErr::NoMem);
        }

        chunk_write_active_ = true;
        chunk_payload_remaining_ = len;
        chunk_write_end_ = end;

        std::size_t prefix_remaining = prefix_len;
        while (prefix_remaining != 0) {
            auto written = co_await connection_->transport().writev(chunk, timeout);
            if (!written) {
                co_return std::unexpected(written.error());
            }
            if (*written == 0) {
                co_return std::unexpected(common::IoErr::ConnReset);
            }
            if (*written >= prefix_remaining) {
                const std::size_t payload_written = *written - prefix_remaining;
                prefix_remaining = 0;
                if (payload_written != 0) {
                    chunk_payload_remaining_ -= payload_written;
                    response_body_sent_ += payload_written;
                    response_phase_ = ResponsePhase::BodyStreaming;
                    if (chunk_payload_remaining_ != 0) {
                        co_return payload_written;
                    }
                    chunk_suffix_pending_ = true;
                    auto suffix = co_await write_chunk_suffix(end, timeout);
                    if (!suffix) {
                        co_return std::unexpected(suffix.error());
                    }
                    chunk_write_active_ = false;
                    chunk_write_end_ = false;
                    chunk_suffix_pending_ = false;
                    if (end) {
                        chunk.clear_complete();
                        response_phase_ = ResponsePhase::Finished;
                    }
                    co_return payload_written;
                }
            } else {
                prefix_remaining -= *written;
            }
        }
    }

    auto written = co_await connection_->transport().writev(chunk, timeout);
    if (!written) {
        co_return std::unexpected(written.error());
    }
    if (*written == 0) {
        co_return std::unexpected(common::IoErr::ConnReset);
    }
    chunk_payload_remaining_ -= *written;
    response_body_sent_ += *written;
    response_phase_ = ResponsePhase::BodyStreaming;
    if (chunk_payload_remaining_ != 0) {
        co_return *written;
    }
    chunk_suffix_pending_ = true;
    auto suffix = co_await write_chunk_suffix(end, timeout);
    if (!suffix) {
        co_return std::unexpected(suffix.error());
    }
    chunk_write_active_ = false;
    chunk_write_end_ = false;
    chunk_suffix_pending_ = false;
    if (end) {
        chunk.clear_complete();
        response_phase_ = ResponsePhase::Finished;
    }
    co_return *written;
}

fiber::async::Task<common::IoResult<size_t>> Http1ExchangeIo::write(HttpExchange &exchange, const uint8_t *buf,
                                                                    size_t len, bool end,
                                                                    std::chrono::milliseconds timeout) noexcept {
    if ((len != 0 && buf == nullptr) || !connection_ || exchange.io_ != this ||
        response_phase_ == ResponsePhase::Init) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (response_phase_ == ResponsePhase::Finished) {
        co_return std::unexpected(common::IoErr::Already);
    }

    if (response_body_spec_.is_none()) {
        if (len != 0 || end) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
        co_return 0;
    }

    if (response_body_spec_.is_content_length()) {
        if (response_body_sent_ > response_content_length_) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
        const std::size_t remaining = response_content_length_ - response_body_sent_;
        if (len > remaining || (end && len != remaining)) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
    }

    if (!response_body_spec_.is_chunked()) {
        if (len == 0) {
            if (end) {
                response_phase_ = ResponsePhase::Finished;
            }
            co_return 0;
        }
        auto written = co_await connection_->transport().write(buf, len, timeout);
        if (!written) {
            co_return std::unexpected(written.error());
        }
        if (*written == 0) {
            co_return std::unexpected(common::IoErr::ConnReset);
        }
        response_body_sent_ += *written;
        response_phase_ = ResponsePhase::BodyStreaming;
        if (*written == len && end) {
            response_phase_ = ResponsePhase::Finished;
        }
        co_return *written;
    }

    if (chunk_write_active_) {
        if (len != chunk_payload_remaining_ || end != chunk_write_end_) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
        if (chunk_suffix_pending_) {
            auto suffix = co_await write_chunk_suffix(end, timeout);
            if (!suffix) {
                co_return std::unexpected(suffix.error());
            }
            chunk_write_active_ = false;
            chunk_write_end_ = false;
            chunk_suffix_pending_ = false;
            if (end) {
                response_phase_ = ResponsePhase::Finished;
            }
            co_return 0;
        }
    } else {
        if (len == 0) {
            if (!end) {
                co_return 0;
            }
            auto finished = co_await transport_write_all(&connection_->transport(), "0\r\n\r\n", 5, timeout);
            if (!finished) {
                co_return std::unexpected(finished.error());
            }
            response_phase_ = ResponsePhase::Finished;
            co_return 0;
        }

        std::array<char, kMaxChunkSizeHexDigits + 2> prefix_buf{};
        char *prefix_ptr = prefix_buf.data();
        prefix_ptr += append_hex(prefix_ptr, len);
        *prefix_ptr++ = '\r';
        *prefix_ptr++ = '\n';
        const std::size_t prefix_len = static_cast<std::size_t>(prefix_ptr - prefix_buf.data());
        auto prefix = co_await transport_write_all(&connection_->transport(), prefix_buf.data(), prefix_len, timeout);
        if (!prefix) {
            co_return std::unexpected(prefix.error());
        }
        chunk_write_active_ = true;
        chunk_payload_remaining_ = len;
        chunk_write_end_ = end;
    }

    auto written = co_await connection_->transport().write(buf, len, timeout);
    if (!written) {
        co_return std::unexpected(written.error());
    }
    if (*written == 0) {
        co_return std::unexpected(common::IoErr::ConnReset);
    }
    chunk_payload_remaining_ -= *written;
    response_body_sent_ += *written;
    response_phase_ = ResponsePhase::BodyStreaming;
    if (chunk_payload_remaining_ != 0) {
        co_return *written;
    }
    chunk_suffix_pending_ = true;
    auto suffix = co_await write_chunk_suffix(end, timeout);
    if (!suffix) {
        co_return std::unexpected(suffix.error());
    }
    chunk_write_active_ = false;
    chunk_write_end_ = false;
    chunk_suffix_pending_ = false;
    if (end) {
        response_phase_ = ResponsePhase::Finished;
    }
    co_return *written;
}

bool Http1ExchangeIo::should_keep_alive(const HttpExchange &) const noexcept {
    return response_phase_ == ResponsePhase::Finished && !response_body_spec_.is_stream() && !close_after_response_;
}

common::IoResult<void> Http1ExchangeIo::abort(HttpExchange &exchange, common::IoErr) noexcept {
    if (!connection_ || exchange.io_ != this) {
        return std::unexpected(common::IoErr::Invalid);
    }
    connection_->transport().close();
    response_phase_ = ResponsePhase::Finished;
    close_after_response_ = true;
    return {};
}

} // namespace fiber::http
