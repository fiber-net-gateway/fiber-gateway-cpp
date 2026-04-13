#include "Http1Connection.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <limits>
#include <system_error>
#include <utility>

#include "HeaderMap.h"
#include "Http1ExchangeIo.h"
#include "Http1HeaderParseBuffer.h"
#include "Http1Server.h"
#include "HttpTransport.h"

namespace fiber::http {

namespace {

constexpr std::size_t kBodyReadChunk = 4096;

unsigned char ascii_lower(unsigned char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return static_cast<unsigned char>(ch + ('a' - 'A'));
    }
    return ch;
}

bool equals_ascii_ci(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (ascii_lower(static_cast<unsigned char>(a[i])) != ascii_lower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::string_view trim_lws(std::string_view value) {
    while (!value.empty()) {
        char ch = value.front();
        if (ch != ' ' && ch != '\t') {
            break;
        }
        value.remove_prefix(1);
    }
    while (!value.empty()) {
        char ch = value.back();
        if (ch != ' ' && ch != '\t') {
            break;
        }
        value.remove_suffix(1);
    }
    return value;
}

template<typename F>
void for_each_token(std::string_view value, F &&fn) {
    size_t start = 0;
    while (start < value.size()) {
        size_t comma = value.find(',', start);
        size_t end = comma == std::string_view::npos ? value.size() : comma;
        std::string_view token = trim_lws(value.substr(start, end - start));
        if (!token.empty()) {
            fn(token);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
}

Http1HeaderParseBufferOptions header_parse_buffer_options(const HttpServerOptions &options) noexcept {
    return Http1HeaderParseBufferOptions{
            .init_size = options.header_init_size,
            .large_size = options.header_large_size,
            .large_num = options.header_large_num,
    };
}

} // namespace

Http1Connection::Http1Connection(Http1Server *server, std::unique_ptr<HttpTransport> transport, HttpHandler handler,
                                 HttpServerOptions options) :
    server_(server), loop_(event::EventLoop::current()), transport_(std::move(transport)), handler_(std::move(handler)),
    options_(std::move(options)) {}

Http1Connection::~Http1Connection() {
    if (transport_ && transport_->valid() && loop_.in_loop()) {
        transport_->close();
    }
}

fiber::async::Task<common::IoResult<size_t>>
Http1Connection::read_into_inbound(std::chrono::milliseconds timeout) noexcept {
    mem::IoBuf buf = mem::IoBuf::allocate(kBodyReadChunk);
    if (!buf) {
        co_return std::unexpected(common::IoErr::NoMem);
    }
    auto result = co_await transport_->read_into(buf, timeout);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    if (*result == 0) {
        co_return static_cast<size_t>(0);
    }
    if (!inbound_bufs_.append(std::move(buf))) {
        co_return std::unexpected(common::IoErr::NoMem);
    }
    co_return *result;
}

bool Http1Connection::handle_content_length(HttpExchange &exchange, const HttpHeaders::HeaderField &header) {
    std::string_view value = header.value_view();
    unsigned long long parsed = 0;
    auto result = std::from_chars(value.data(), value.data() + value.size(), parsed, 10);
    if (result.ec != std::errc() || result.ptr != value.data() + value.size()) {
        return false;
    }
    if (parsed > static_cast<unsigned long long>(std::numeric_limits<size_t>::max())) {
        return false;
    }
    size_t length = static_cast<size_t>(parsed);
    if (exchange.request_content_length_set_ && exchange.request_content_length_ != length) {
        return false;
    }
    if (!exchange.request_chunked_) {
        exchange.request_content_length_set_ = true;
        exchange.request_content_length_ = length;
    }
    return true;
}

bool Http1Connection::handle_transfer_encoding(HttpExchange &exchange, const HttpHeaders::HeaderField &header) {
    std::string_view value = header.value_view();
    bool chunked = false;
    for_each_token(value, [&](std::string_view token) {
        if (equals_ascii_ci(token, "chunked")) {
            chunked = true;
        }
    });
    if (chunked) {
        exchange.request_chunked_ = true;
        exchange.request_content_length_set_ = false;
        exchange.request_content_length_ = 0;
    }
    return true;
}

bool Http1Connection::handle_connection(HttpExchange &exchange, const HttpHeaders::HeaderField &header) {
    std::string_view value = header.value_view();
    for_each_token(value, [&](std::string_view token) {
        if (equals_ascii_ci(token, "close")) {
            exchange.request_close_ = true;
        } else if (equals_ascii_ci(token, "keep-alive")) {
            exchange.request_keep_alive_ = true;
        }
    });
    return true;
}

const HeaderMap<Http1Connection::HeaderHandler> &Http1Connection::header_handler_map() {
    static HeaderMap<HeaderHandler> handlers = []() {
        HeaderMap<HeaderHandler> map;
        map.insert("content-length", &Http1Connection::handle_content_length);
        map.insert("transfer-encoding", &Http1Connection::handle_transfer_encoding);
        map.insert("connection", &Http1Connection::handle_connection);
        return map;
    }();
    return handlers;
}

std::size_t Http1Connection::drain_inbound(mem::IoBuf &buffer) noexcept {
    std::size_t copied = 0;
    while (buffer.writable() > 0) {
        mem::IoBuf *front = inbound_bufs_.front();
        if (!front || front->readable() == 0) {
            break;
        }
        std::size_t take = std::min(front->readable(), buffer.writable());
        std::memcpy(buffer.writable_data(), front->readable_data(), take);
        buffer.commit(take);
        inbound_bufs_.consume_and_compact(take);
        copied += take;
    }
    return copied;
}

fiber::async::Task<fiber::common::IoResult<ParseCode>> Http1Connection::parse_request(HttpExchange &exchange) {
    Http1HeaderParseBuffer header_buffer(header_parse_buffer_options(options_));
    auto init_result = header_buffer.ensure_init();
    if (!init_result) {
        co_return std::unexpected(init_result.error());
    }

    {
        RequestLineParser req_parser(options_);
        for (;;) {
            ParseCode code = req_parser.execute(&header_buffer.buf());
            if (code == ParseCode::Again) {
                if (header_buffer.buf().writable() == 0) {
                    if (!header_buffer.can_grow()) {
                        co_return ParseCode::HeaderTooLarge;
                    }
                    auto grow_result = header_buffer.grow_and_replace(req_parser);
                    if (!grow_result) {
                        co_return std::unexpected(grow_result.error());
                    }
                }
                std::size_t copied = drain_inbound(header_buffer.buf());
                if (copied == 0) {
                    auto timeout =
                            header_buffer.buf().readable() == 0 ? options_.keep_alive_timeout : options_.header_timeout;
                    auto result = co_await transport_->read_into(header_buffer.buf(), timeout);
                    if (!result) {
                        co_return std::unexpected(result.error());
                    }
                    if (*result == 0) {
                        co_return std::unexpected(common::IoErr::ConnReset);
                    }
                }
                continue;
            }
            if (code != ParseCode::Ok) {
                co_return code;
            }

            const auto &line = req_parser.state();
            exchange.method_ = line.method;
            if (line.request_start && line.method_end && line.method_end >= line.request_start) {
                std::size_t method_len = static_cast<size_t>(line.method_end - line.request_start + 1);
                exchange.method_view_ = std::string_view(reinterpret_cast<char *>(line.request_start), method_len);
            } else {
                exchange.method_view_ = {};
            }

            exchange.version_ = static_cast<HttpVersion>(line.http_version);
            if (line.http_protocol_start && line.request_end && line.request_end >= line.http_protocol_start) {
                std::size_t version_len = static_cast<size_t>(line.request_end - line.http_protocol_start);
                exchange.version_view_ =
                        std::string_view(reinterpret_cast<char *>(line.http_protocol_start), version_len);
            } else {
                exchange.version_view_ = {};
            }

            if (line.uri_start && line.uri_end && line.uri_end >= line.uri_start) {
                std::size_t uri_len = static_cast<std::size_t>(line.uri_end - line.uri_start);
                exchange.uri_.unparsed_uri = std::string_view(reinterpret_cast<char *>(line.uri_start), uri_len);
                if (line.args_start && line.args_start <= line.uri_end) {
                    std::size_t path_len = static_cast<size_t>(line.args_start - line.uri_start - 1);
                    exchange.uri_.path = std::string_view(reinterpret_cast<char *>(line.uri_start), path_len);
                    std::size_t query_len = static_cast<size_t>(line.uri_end - line.args_start);
                    exchange.uri_.query = std::string_view(reinterpret_cast<char *>(line.args_start), query_len);
                } else {
                    exchange.uri_.path = exchange.uri_.unparsed_uri;
                }
                if (line.uri_ext && line.uri_ext < line.uri_end) {
                    size_t ext_len = static_cast<size_t>(line.uri_end - line.uri_ext);
                    exchange.uri_.exten = std::string_view(reinterpret_cast<char *>(line.uri_ext), ext_len);
                }
            }
            break;
        }
    }

    {
        HeaderLineParser hdr_parser(options_);
        for (;;) {
            ParseCode code = hdr_parser.execute(&header_buffer.buf());
            if (code == ParseCode::Again) {
                if (header_buffer.buf().writable() == 0) {
                    if (!header_buffer.can_grow()) {
                        co_return ParseCode::HeaderTooLarge;
                    }
                    auto grow_result = header_buffer.grow_and_replace(hdr_parser);
                    if (!grow_result) {
                        co_return std::unexpected(grow_result.error());
                    }
                }
                std::size_t copied = drain_inbound(header_buffer.buf());
                if (copied == 0) {
                    auto timeout =
                            header_buffer.buf().readable() == 0 ? options_.keep_alive_timeout : options_.header_timeout;
                    auto result = co_await transport_->read_into(header_buffer.buf(), timeout);
                    if (!result) {
                        co_return std::unexpected(result.error());
                    }
                    if (*result == 0) {
                        co_return std::unexpected(common::IoErr::ConnReset);
                    }
                }
                continue;
            }
            if (code == ParseCode::Ok) {
                const auto &line = hdr_parser.state();
                if (!line.header_name_start || !line.header_name_end || line.header_name_end < line.header_name_start) {
                    co_return ParseCode::InvalidHeader;
                }
                std::size_t name_len = static_cast<std::size_t>(line.header_name_end - line.header_name_start);
                std::string_view name(reinterpret_cast<char *>(line.header_name_start), name_len);
                std::string_view value;
                if (line.header_start && line.header_end && line.header_end >= line.header_start) {
                    std::size_t value_len = static_cast<std::size_t>(line.header_end - line.header_start);
                    value = std::string_view(reinterpret_cast<char *>(line.header_start), value_len);
                }
                char *lowercase = static_cast<char *>(exchange.pool_.alloc(name_len));
                if (lowercase == nullptr) {
                    co_return std::unexpected(common::IoErr::NoMem);
                }
                if (line.lowcase_index == name_len) {
                    ::memcpy(lowercase, line.lowcase_header, name_len);
                } else {
                    to_lowercase(name, lowercase);
                }

                uint32_t hash = line.header_hash;
                HttpHeaders::HeaderField *field = exchange.request_headers_.add_view(name, value, lowercase, hash);
                if (!field) {
                    co_return std::unexpected(common::IoErr::NoMem);
                }
                exchange.cache_request_header_field(*field);
                if (auto *handler = header_handler_map().get(std::string_view(lowercase, name_len), hash)) {
                    if (!(*handler)(exchange, *field)) {
                        co_return ParseCode::InvalidHeader;
                    }
                }
                continue;
            }
            if (code == ParseCode::HeaderDone) {
                const std::size_t header_bytes =
                        static_cast<std::size_t>(header_buffer.buf().readable_data() - header_buffer.buf().data());
                auto header_owner_result = header_buffer.retain_prefix(header_bytes);
                if (!header_owner_result) {
                    co_return std::unexpected(header_owner_result.error());
                }
                mem::IoBuf header_owner = std::move(*header_owner_result);
                if (!exchange.header_bufs_.append(std::move(header_owner))) {
                    co_return std::unexpected(common::IoErr::NoMem);
                }
                if (header_buffer.buf().readable() > 0) {
                    auto trailing_result = header_buffer.retain_suffix();
                    if (!trailing_result) {
                        co_return std::unexpected(trailing_result.error());
                    }
                    mem::IoBuf trailing = std::move(*trailing_result);
                    if (!inbound_bufs_.append(std::move(trailing))) {
                        co_return std::unexpected(common::IoErr::NoMem);
                    }
                }
                co_return ParseCode::Ok;
            }
            if (code != ParseCode::Again) {
                co_return code;
            }
        }
    }
}

fiber::async::Task<void> Http1Connection::run() {
    struct FinishGuard {
        Http1Connection *conn = nullptr;
        ~FinishGuard() {
            if (conn) {
                conn->finish();
            }
        }
    } finish_guard{this};

    if (!transport_ || !transport_->valid()) {
        co_return;
    }

    for (;;) {
        if (server_ && server_->shutting_down()) {
            break;
        }

        HttpExchange exchange(options_);
        auto parse_result = co_await parse_request(exchange);
        if (!parse_result) {
            break;
        }
        if (*parse_result != ParseCode::Ok) {
            break;
        }

        {
            Http1ExchangeIo io(*this, exchange);
            exchange.set_io(&io);

            co_await handler_(exchange);

            if (!io.request_body_complete()) {
                if (options_.drain_unread_body) {
                    auto discard_result = co_await exchange.discard_body();
                    if (!discard_result) {
                        exchange.set_io(nullptr);
                        break;
                    }
                } else {
                    exchange.set_io(nullptr);
                    break;
                }
            }

            if (!io.response_complete()) {
                exchange.set_io(nullptr);
                break;
            }

            if ((server_ && server_->shutting_down()) || !io.should_keep_alive(exchange)) {
                exchange.set_io(nullptr);
                break;
            }

            exchange.set_io(nullptr);
        }
    }

    if (transport_) {
        auto shutdown_result = co_await transport_->shutdown(options_.write_timeout);
        (void) shutdown_result;
    }
    co_return;
}

bool Http1Connection::stopping() const noexcept { return server_ && server_->shutting_down(); }

void Http1Connection::finish() noexcept {
    if (finished_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (transport_) {
        transport_->close();
    }
}

} // namespace fiber::http
