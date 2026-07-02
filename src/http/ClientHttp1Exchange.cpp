#include "ClientHttp1Exchange.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <limits>
#include <system_error>
#include <utility>

#include "../event/EventLoop.h"
#include "HeaderMap.h"
#include "Http1ClientConnection.h"
#include "HttpHeaderHash.h"
#include "HttpTransport.h"

namespace fiber::http {

namespace {

constexpr std::string_view kHttp11Suffix = " HTTP/1.1\r\n";
constexpr std::string_view kHeaderNameValueSep = ": ";
constexpr std::string_view kLineTerminator = "\r\n";
constexpr std::string_view kContentLengthPrefix = "Content-Length: ";
constexpr std::string_view kChunkedHeader = "Transfer-Encoding: chunked\r\n";
constexpr std::string_view kChunkedFinal = "0\r\n\r\n";
constexpr std::string_view kChunkedLastPrefix = "0\r\n";
constexpr std::size_t kMaxContentLengthDigits = 20;
constexpr std::size_t kMaxChunkSizeHexDigits = sizeof(std::size_t) * 2;
constexpr std::size_t kMaxDirectBodyRead = 64 * 1024;
struct ResponseHeaderParseState {
    bool content_length_set = false;
    std::size_t content_length = 0;
    bool chunked = false;
    bool connection_close = false;
    bool connection_keep_alive = false;
};

using ResponseHeaderHandler = bool (*)(ResponseHeaderParseState &, const HttpHeaders::HeaderField &);

std::string_view http_method_name(HttpMethod method) noexcept {
    switch (method) {
        case HttpMethod::Get:
            return "GET";
        case HttpMethod::Head:
            return "HEAD";
        case HttpMethod::Post:
            return "POST";
        case HttpMethod::Put:
            return "PUT";
        case HttpMethod::Delete:
            return "DELETE";
        case HttpMethod::MKCOL:
            return "MKCOL";
        case HttpMethod::Copy:
            return "COPY";
        case HttpMethod::Move:
            return "MOVE";
        case HttpMethod::Options:
            return "OPTIONS";
        case HttpMethod::PropFind:
            return "PROPFIND";
        case HttpMethod::PropPatch:
            return "PROPPATCH";
        case HttpMethod::Lock:
            return "LOCK";
        case HttpMethod::Unlock:
            return "UNLOCK";
        case HttpMethod::Patch:
            return "PATCH";
        case HttpMethod::Trace:
            return "TRACE";
        case HttpMethod::Connect:
            return "CONNECT";
        case HttpMethod::Unknown:
            return {};
    }
    return {};
}

unsigned char ascii_lower(unsigned char ch) noexcept {
    if (ch >= 'A' && ch <= 'Z') {
        return static_cast<unsigned char>(ch + ('a' - 'A'));
    }
    return ch;
}

bool equals_ascii_ci(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (ascii_lower(static_cast<unsigned char>(a[i])) != ascii_lower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::string_view trim_lws(std::string_view value) noexcept {
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
    std::size_t start = 0;
    while (start < value.size()) {
        std::size_t comma = value.find(',', start);
        std::size_t end = comma == std::string_view::npos ? value.size() : comma;
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

HttpVersion to_http_version(int version) noexcept {
    switch (version) {
        case 9:
            return HttpVersion::HTTP_0_9;
        case 1000:
            return HttpVersion::HTTP_1_0;
        case 1001:
            return HttpVersion::HTTP_1_1;
        case 2000:
            return HttpVersion::HTTP_2_0;
        case 3000:
            return HttpVersion::HTTP_3_0;
        default:
            return static_cast<HttpVersion>(version);
    }
}

bool response_has_no_body(HttpMethod request_method, int status_code) noexcept {
    return request_method == HttpMethod::Head || (status_code >= 100 && status_code < 200) || status_code == 204 ||
           status_code == 304;
}

bool default_keepalive(HttpVersion version) noexcept { return version >= HttpVersion::HTTP_1_1; }

Http1HeaderParseBufferOptions response_header_buffer_options(const Http1ClientExchangeOptions &options) noexcept {
    return Http1HeaderParseBufferOptions{
            .init_size = options.response_header_init_size,
            .large_size = options.response_header_large_size,
            .large_num = options.response_header_large_num,
    };
}

bool parse_content_length_value(std::string_view value, std::size_t &length) noexcept {
    unsigned long long parsed = 0;
    auto result = std::from_chars(value.data(), value.data() + value.size(), parsed, 10);
    if (result.ec != std::errc() || result.ptr != value.data() + value.size()) {
        return false;
    }
    if (parsed > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
        return false;
    }
    length = static_cast<std::size_t>(parsed);
    return true;
}

bool handle_response_content_length(ResponseHeaderParseState &state, const HttpHeaders::HeaderField &header) noexcept {
    std::size_t parsed_length = 0;
    if (!parse_content_length_value(header.value_view(), parsed_length)) {
        return false;
    }
    if (state.content_length_set && state.content_length != parsed_length) {
        return false;
    }
    if (!state.chunked) {
        state.content_length_set = true;
        state.content_length = parsed_length;
    }
    return true;
}

bool handle_response_transfer_encoding(ResponseHeaderParseState &state,
                                       const HttpHeaders::HeaderField &header) noexcept {
    bool chunked = false;
    for_each_token(header.value_view(), [&](std::string_view token) {
        if (equals_ascii_ci(token, "chunked")) {
            chunked = true;
        }
    });
    if (chunked) {
        state.chunked = true;
        state.content_length_set = false;
        state.content_length = 0;
    }
    return true;
}

bool handle_response_connection(ResponseHeaderParseState &state, const HttpHeaders::HeaderField &header) noexcept {
    for_each_token(header.value_view(), [&](std::string_view token) {
        if (equals_ascii_ci(token, "close")) {
            state.connection_close = true;
        } else if (equals_ascii_ci(token, "keep-alive")) {
            state.connection_keep_alive = true;
        }
    });
    return true;
}

const HeaderMap<ResponseHeaderHandler> &response_header_handler_map() {
    static HeaderMap<ResponseHeaderHandler> handlers = []() {
        HeaderMap<ResponseHeaderHandler> map;
        map.insert("content-length", &handle_response_content_length);
        map.insert("transfer-encoding", &handle_response_transfer_encoding);
        map.insert("connection", &handle_response_connection);
        return map;
    }();
    return handlers;
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

common::IoResult<std::size_t> estimate_header_bytes(const Http1RequestHead &head) noexcept {
    std::string_view method = http_method_name(head.method);
    if (method.empty() || head.target.empty()) {
        return std::unexpected(common::IoErr::Invalid);
    }

    std::size_t total = method.size() + 1 + head.target.size() + kHttp11Suffix.size();
    switch (head.body.kind()) {
        case HttpBodySpec::Kind::Auto:
            return std::unexpected(common::IoErr::Invalid);
        case HttpBodySpec::Kind::None:
            break;
        case HttpBodySpec::Kind::ContentLength:
            if (head.body.content_length() > static_cast<std::size_t>(std::numeric_limits<std::uint64_t>::max())) {
                return std::unexpected(common::IoErr::Invalid);
            }
            total += kContentLengthPrefix.size() + kMaxContentLengthDigits + kLineTerminator.size();
            break;
        case HttpBodySpec::Kind::Chunked:
            total += kChunkedHeader.size();
            break;
    }

    if (head.headers) {
        for (const auto &field: *head.headers) {
            if (field.name_len > std::numeric_limits<std::size_t>::max() - total - kHeaderNameValueSep.size() -
                                         field.value_len - kLineTerminator.size()) {
                return std::unexpected(common::IoErr::NoMem);
            }
            total += field.name_len + kHeaderNameValueSep.size() + field.value_len + kLineTerminator.size();
        }
    }
    if (total > std::numeric_limits<std::size_t>::max() - kLineTerminator.size()) {
        return std::unexpected(common::IoErr::NoMem);
    }
    total += kLineTerminator.size();
    return total;
}

common::IoResult<void> encode_request_header(mem::IoBuf &buf, const Http1RequestHead &head) noexcept {
    std::string_view method = http_method_name(head.method);
    if (method.empty() || head.target.empty()) {
        return std::unexpected(common::IoErr::Invalid);
    }

    char *out = reinterpret_cast<char *>(buf.writable_data());
    char *ptr = out;
    append_bytes(ptr, method);
    *ptr++ = ' ';
    append_bytes(ptr, head.target);
    append_bytes(ptr, kHttp11Suffix);

    switch (head.body.kind()) {
        case HttpBodySpec::Kind::Auto:
            return std::unexpected(common::IoErr::Invalid);
        case HttpBodySpec::Kind::None:
            break;
        case HttpBodySpec::Kind::ContentLength:
            append_bytes(ptr, kContentLengthPrefix);
            ptr += append_decimal(ptr, head.body.content_length());
            append_bytes(ptr, kLineTerminator);
            break;
        case HttpBodySpec::Kind::Chunked:
            append_bytes(ptr, kChunkedHeader);
            break;
    }

    if (head.headers) {
        for (const auto &field: *head.headers) {
            append_bytes(ptr, field.name_view());
            append_bytes(ptr, kHeaderNameValueSep);
            append_bytes(ptr, field.value_view());
            append_bytes(ptr, kLineTerminator);
        }
    }
    append_bytes(ptr, kLineTerminator);
    buf.commit(static_cast<std::size_t>(ptr - out));
    return {};
}

common::IoResult<std::size_t> estimate_trailer_bytes(const HttpHeaders &trailers) noexcept {
    std::size_t total = kChunkedLastPrefix.size() + kLineTerminator.size();
    for (const auto &field: trailers) {
        if (field.name_len > std::numeric_limits<std::size_t>::max() - total - kHeaderNameValueSep.size() -
                                     field.value_len - kLineTerminator.size()) {
            return std::unexpected(common::IoErr::NoMem);
        }
        total += field.name_len + kHeaderNameValueSep.size() + field.value_len + kLineTerminator.size();
    }
    return total;
}

common::IoResult<void> encode_chunked_trailer(mem::IoBuf &buf, const HttpHeaders &trailers) noexcept {
    char *out = reinterpret_cast<char *>(buf.writable_data());
    char *ptr = out;
    append_bytes(ptr, kChunkedLastPrefix);
    for (const auto &field: trailers) {
        append_bytes(ptr, field.name_view());
        append_bytes(ptr, kHeaderNameValueSep);
        append_bytes(ptr, field.value_view());
        append_bytes(ptr, kLineTerminator);
    }
    append_bytes(ptr, kLineTerminator);
    buf.commit(static_cast<std::size_t>(ptr - out));
    return {};
}

fiber::async::Task<common::IoResult<void>> write_all(HttpTransport *transport, const void *buf, std::size_t len,
                                                     std::chrono::milliseconds timeout) {
    const auto *ptr = static_cast<const std::uint8_t *>(buf);
    std::size_t remaining = len;
    while (remaining > 0) {
        auto write_result = co_await transport->write(ptr, remaining, timeout);
        if (!write_result) {
            co_return std::unexpected(write_result.error());
        }
        if (*write_result == 0) {
            co_return std::unexpected(common::IoErr::ConnReset);
        }
        ptr += *write_result;
        remaining -= *write_result;
    }
    co_return common::IoResult<void>{};
}

fiber::async::Task<common::IoResult<void>> write_all(HttpTransport *transport, mem::IoBufChain &chain,
                                                     std::chrono::milliseconds timeout) {
    while (chain.readable_bytes() > 0) {
        auto write_result = co_await transport->writev(chain, timeout);
        if (!write_result) {
            co_return std::unexpected(write_result.error());
        }
        if (*write_result == 0) {
            co_return std::unexpected(common::IoErr::ConnReset);
        }
    }
    co_return common::IoResult<void>{};
}

} // namespace

ClientHttp1Exchange::ClientHttp1Exchange(Http1ClientConnection &conn, mem::BufPool &pool,
                                         Http1ClientExchangeOptions options) noexcept :
    conn_(&conn), pool_(&pool), options_(std::move(options)), response_trailers_(pool) {
    active_ = conn.acquire_exchange(this);
}

ClientHttp1Exchange::~ClientHttp1Exchange() {
    if (!conn_ || !active_) {
        clear_response_header_nodes();
        return;
    }
    if (done()) {
        conn_->release_exchange(this, keepalive_on_release_);
    } else {
        conn_->fail_exchange(this);
    }
    clear_response_header_nodes();
}

void ClientHttp1Exchange::clear_response_header_nodes() noexcept {
    ResponseHeaderNode *node = response_headers_head_;
    while (node) {
        ResponseHeaderNode *next = node->next;
        node->~ResponseHeaderNode();
        ResponseHeaderNode::operator delete(node);
        node = next;
    }
    response_headers_head_ = nullptr;
}

void ClientHttp1Exchange::fail_active_exchange() noexcept {
    request_state_ = RequestState::Failed;
    active_ = false;
    if (conn_) {
        conn_->fail_exchange(this);
        conn_ = nullptr;
    }
}

common::IoResult<void> ClientHttp1Exchange::ensure_body_read_buf_writable(mem::IoBuf &read_buf,
                                                                          std::size_t min_writable) noexcept {
    if (min_writable == 0) {
        return {};
    }

    if (!read_buf) {
        read_buf = mem::IoBuf::allocate(min_writable);
        if (!read_buf) {
            return std::unexpected(common::IoErr::NoMem);
        }
        return {};
    }

    if (read_buf.readable() == 0) {
        if (read_buf.unique() && read_buf.capacity() >= min_writable) {
            read_buf.reset();
            return {};
        }

        mem::IoBuf next = mem::IoBuf::allocate(min_writable);
        if (!next) {
            return std::unexpected(common::IoErr::NoMem);
        }
        read_buf = std::move(next);
        return {};
    }

    if (read_buf.writable() >= min_writable) {
        return {};
    }

    std::size_t unread = read_buf.readable();
    mem::IoBuf next = mem::IoBuf::allocate(unread + min_writable);
    if (!next) {
        return std::unexpected(common::IoErr::NoMem);
    }
    std::memcpy(next.writable_data(), read_buf.readable_data(), unread);
    next.commit(unread);
    read_buf = std::move(next);
    return {};
}

common::IoResult<void> ClientHttp1Exchange::take_prefix(mem::IoBuf &read_buf, mem::IoBufChain &out,
                                                        std::size_t len) noexcept {
    if (len > read_buf.readable()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    mem::IoBuf piece = read_buf.retain_slice(0, len);
    if (!piece) {
        return std::unexpected(common::IoErr::NoMem);
    }
    if (!out.append(std::move(piece))) {
        return std::unexpected(common::IoErr::NoMem);
    }
    read_buf.consume(len);
    return {};
}

common::IoResult<void> ClientHttp1Exchange::stash_pending_buf(mem::IoBuf &read_buf) noexcept {
    if (read_buf.readable() == 0) {
        pending_buf_ = {};
        return {};
    }

    mem::IoBuf pending = read_buf.retain_slice(0, read_buf.readable());
    if (!pending) {
        return std::unexpected(common::IoErr::NoMem);
    }
    pending_buf_ = std::move(pending);
    return {};
}

fiber::async::Task<common::IoResult<std::size_t>>
ClientHttp1Exchange::read_more(mem::IoBuf &read_buf, std::size_t max_bytes, bool &read_call_used_io) noexcept {
    if (!conn_ || !conn_->transport_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    std::size_t read_size = std::min(max_bytes, kMaxDirectBodyRead);
    if (read_size == 0) {
        co_return static_cast<std::size_t>(0);
    }

    auto ensure_result = ensure_body_read_buf_writable(read_buf, read_size);
    if (!ensure_result) {
        co_return std::unexpected(ensure_result.error());
    }

    auto read_result =
            co_await conn_->transport_->read(read_buf.writable_data(), read_size, options_.response_body_timeout);
    if (!read_result) {
        co_return std::unexpected(read_result.error());
    }
    read_call_used_io = true;
    read_buf.commit(*read_result);
    co_return *read_result;
}

fiber::async::Task<common::IoResult<ParseCode>>
ClientHttp1Exchange::advance_chunked_body(mem::IoBuf &read_buf, std::size_t max_bytes, bool allow_read,
                                          bool &read_call_used_io) noexcept {
    for (;;) {
        if (read_buf.readable() == 0) {
            if (!allow_read) {
                co_return ParseCode::Again;
            }
            auto more = co_await read_more(read_buf, max_bytes, read_call_used_io);
            if (!more) {
                co_return std::unexpected(more.error());
            }
            if (*more == 0) {
                co_return std::unexpected(common::IoErr::ConnReset);
            }
            allow_read = false;
            continue;
        }

        mem::IoBuf cursor(read_buf);
        ParseCode code = response_body_parser_.execute(&cursor);
        std::size_t consumed = read_buf.readable() - cursor.readable();
        if (consumed > 0) {
            read_buf.consume(consumed);
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

fiber::async::Task<common::IoResult<void>> ClientHttp1Exchange::read_response_trailers(mem::IoBuf &read_buf) noexcept {
    if (!conn_ || !conn_->transport_ || pool_ == nullptr) {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    Http1HeaderParseBuffer header_buffer(response_header_buffer_options(options_));
    auto init_result = header_buffer.ensure_init();
    if (!init_result) {
        co_return std::unexpected(init_result.error());
    }

    auto drain_read_buf = [&]() -> common::IoResult<void> {
        while (read_buf.readable() > 0 && header_buffer.buf().writable() > 0) {
            std::size_t take = std::min(read_buf.readable(), header_buffer.buf().writable());
            std::memcpy(header_buffer.buf().writable_data(), read_buf.readable_data(), take);
            header_buffer.buf().commit(take);
            read_buf.consume(take);
        }
        return {};
    };

    auto seed_result = drain_read_buf();
    if (!seed_result) {
        co_return std::unexpected(seed_result.error());
    }

    HeaderLineParser parser;
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

            auto copied_result = drain_read_buf();
            if (!copied_result) {
                co_return std::unexpected(copied_result.error());
            }
            if (read_buf.readable() == 0) {
                auto read_result =
                        co_await conn_->transport_->read_into(header_buffer.buf(), options_.response_body_timeout);
                if (!read_result) {
                    co_return std::unexpected(read_result.error());
                }
                if (*read_result == 0) {
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
            std::string_view name(reinterpret_cast<const char *>(line.header_name_start), name_len);
            std::string_view value;
            if (line.header_start && line.header_end && line.header_end >= line.header_start) {
                value = std::string_view(reinterpret_cast<const char *>(line.header_start),
                                         static_cast<std::size_t>(line.header_end - line.header_start));
            }

            if (!response_trailers_.add(name, value)) {
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
                pending_buf_ = std::move(*trailing_result);
            }
            response_body_parser_.finish_chunked_trailers();
            response_complete_ = true;
            co_return common::IoResult<void>{};
        }

        co_return std::unexpected(common::IoErr::Invalid);
    }
}

fiber::async::Task<common::IoResult<void>> ClientHttp1Exchange::send_header(const Http1RequestHead &head,
                                                                            bool end_stream) noexcept {
    if (!active_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (!conn_ || !conn_->transport_ || !conn_->valid()) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (request_state_ != RequestState::Init) {
        co_return std::unexpected(common::IoErr::Already);
    }
    if (head.body.is_auto()) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (head.body.is_none() && !end_stream) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (head.body.is_content_length() && end_stream && head.body.content_length() != 0) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    auto estimated_size = estimate_header_bytes(head);
    if (!estimated_size) {
        co_return std::unexpected(estimated_size.error());
    }

    mem::IoBuf header_buf = mem::IoBuf::allocate(*estimated_size);
    if (!header_buf) {
        co_return std::unexpected(common::IoErr::NoMem);
    }
    auto encode_result = encode_request_header(header_buf, head);
    if (!encode_result) {
        co_return std::unexpected(encode_result.error());
    }

    auto write_result = co_await write_all(conn_->transport_.get(), header_buf.readable_data(), header_buf.readable(),
                                           options_.write_timeout);
    if (!write_result) {
        request_state_ = RequestState::Failed;
        active_ = false;
        conn_->fail_exchange(this);
        conn_ = nullptr;
        co_return std::unexpected(write_result.error());
    }

    if (head.body.is_chunked() && end_stream) {
        auto final_result = co_await write_all(conn_->transport_.get(), kChunkedFinal.data(), kChunkedFinal.size(),
                                               options_.write_timeout);
        if (!final_result) {
            request_state_ = RequestState::Failed;
            active_ = false;
            conn_->fail_exchange(this);
            conn_ = nullptr;
            co_return std::unexpected(final_result.error());
        }
    }

    body_spec_ = head.body;
    content_length_ = head.body.is_content_length() ? head.body.content_length() : 0;
    body_sent_ = 0;
    request_method_ = head.method;
    final_response_received_ = false;
    response_complete_ = false;
    keepalive_on_release_ = false;
    saw_connection_close_ = false;
    saw_connection_keep_alive_ = false;
    response_eof_delimited_ = false;
    pending_buf_ = {};
    clear_response_header_nodes();
    response_trailers_.clear();
    response_body_parser_.reset();
    request_state_ = end_stream ? RequestState::RequestDone : RequestState::SendingBody;
    co_return common::IoResult<void>{};
}

fiber::async::Task<common::IoResult<std::size_t>> ClientHttp1Exchange::write_body(mem::IoBufChain chunk) noexcept {
    if (!active_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (!conn_ || !conn_->transport_ || !conn_->valid()) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (request_state_ == RequestState::Init) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (request_state_ == RequestState::RequestDone) {
        co_return std::unexpected(common::IoErr::Already);
    }
    if (request_state_ == RequestState::Failed) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (final_response_received_) {
        co_return std::unexpected(common::IoErr::Already);
    }

    const std::size_t body_bytes = chunk.readable_bytes();
    switch (body_spec_.kind()) {
        case HttpBodySpec::Kind::Auto:
            co_return std::unexpected(common::IoErr::Invalid);
        case HttpBodySpec::Kind::None:
            co_return std::unexpected(common::IoErr::Invalid);
        case HttpBodySpec::Kind::ContentLength: {
            if (body_bytes > content_length_ - body_sent_) {
                co_return std::unexpected(common::IoErr::Invalid);
            }
            if (chunk.complete() && body_sent_ + body_bytes != content_length_) {
                co_return std::unexpected(common::IoErr::Invalid);
            }
            auto write_result = co_await write_all(conn_->transport_.get(), chunk, options_.write_timeout);
            if (!write_result) {
                request_state_ = RequestState::Failed;
                active_ = false;
                conn_->fail_exchange(this);
                conn_ = nullptr;
                co_return std::unexpected(write_result.error());
            }
            body_sent_ += body_bytes;
            if (body_sent_ == content_length_) {
                request_state_ = RequestState::RequestDone;
            }
            co_return body_bytes;
        }
        case HttpBodySpec::Kind::Chunked: {
            if (body_bytes == 0 && !chunk.complete()) {
                co_return static_cast<std::size_t>(0);
            }

            if (body_bytes != 0) {
                std::array<char, kMaxChunkSizeHexDigits + 2> prefix{};
                char *prefix_ptr = prefix.data();
                prefix_ptr += append_hex(prefix_ptr, body_bytes);
                *prefix_ptr++ = '\r';
                *prefix_ptr++ = '\n';

                auto prefix_result = co_await write_all(conn_->transport_.get(), prefix.data(),
                                                        static_cast<std::size_t>(prefix_ptr - prefix.data()),
                                                        options_.write_timeout);
                if (!prefix_result) {
                    request_state_ = RequestState::Failed;
                    active_ = false;
                    conn_->fail_exchange(this);
                    conn_ = nullptr;
                    co_return std::unexpected(prefix_result.error());
                }
                auto body_result = co_await write_all(conn_->transport_.get(), chunk, options_.write_timeout);
                if (!body_result) {
                    request_state_ = RequestState::Failed;
                    active_ = false;
                    conn_->fail_exchange(this);
                    conn_ = nullptr;
                    co_return std::unexpected(body_result.error());
                }
                auto suffix_result = co_await write_all(conn_->transport_.get(), kLineTerminator.data(),
                                                        kLineTerminator.size(), options_.write_timeout);
                if (!suffix_result) {
                    request_state_ = RequestState::Failed;
                    active_ = false;
                    conn_->fail_exchange(this);
                    conn_ = nullptr;
                    co_return std::unexpected(suffix_result.error());
                }
                body_sent_ += body_bytes;
            }

            if (chunk.complete()) {
                auto final_result = co_await write_all(conn_->transport_.get(), kChunkedFinal.data(),
                                                       kChunkedFinal.size(), options_.write_timeout);
                if (!final_result) {
                    request_state_ = RequestState::Failed;
                    active_ = false;
                    conn_->fail_exchange(this);
                    conn_ = nullptr;
                    co_return std::unexpected(final_result.error());
                }
                request_state_ = RequestState::RequestDone;
            }
            co_return body_bytes;
        }
    }
    co_return std::unexpected(common::IoErr::Invalid);
}

fiber::async::Task<common::IoResult<std::size_t>>
ClientHttp1Exchange::write_body(const std::uint8_t *buf, std::size_t len, bool end_stream) noexcept {
    if (!active_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (len != 0 && buf == nullptr) {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    if (!conn_ || !conn_->transport_ || !conn_->valid()) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (request_state_ == RequestState::Init) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (request_state_ == RequestState::RequestDone) {
        co_return std::unexpected(common::IoErr::Already);
    }
    if (request_state_ == RequestState::Failed) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (final_response_received_) {
        co_return std::unexpected(common::IoErr::Already);
    }

    switch (body_spec_.kind()) {
        case HttpBodySpec::Kind::Auto:
            co_return std::unexpected(common::IoErr::Invalid);
        case HttpBodySpec::Kind::None:
            co_return std::unexpected(common::IoErr::Invalid);
        case HttpBodySpec::Kind::ContentLength: {
            if (len > content_length_ - body_sent_) {
                co_return std::unexpected(common::IoErr::Invalid);
            }
            if (end_stream && body_sent_ + len != content_length_) {
                co_return std::unexpected(common::IoErr::Invalid);
            }
            if (len != 0) {
                auto write_result = co_await write_all(conn_->transport_.get(), buf, len, options_.write_timeout);
                if (!write_result) {
                    request_state_ = RequestState::Failed;
                    active_ = false;
                    conn_->fail_exchange(this);
                    conn_ = nullptr;
                    co_return std::unexpected(write_result.error());
                }
                body_sent_ += len;
            }
            if (body_sent_ == content_length_) {
                request_state_ = RequestState::RequestDone;
            }
            co_return len;
        }
        case HttpBodySpec::Kind::Chunked: {
            if (len == 0 && !end_stream) {
                co_return static_cast<std::size_t>(0);
            }

            if (len != 0) {
                std::array<char, kMaxChunkSizeHexDigits + 2> prefix{};
                char *prefix_ptr = prefix.data();
                prefix_ptr += append_hex(prefix_ptr, len);
                *prefix_ptr++ = '\r';
                *prefix_ptr++ = '\n';

                auto prefix_result = co_await write_all(conn_->transport_.get(), prefix.data(),
                                                        static_cast<std::size_t>(prefix_ptr - prefix.data()),
                                                        options_.write_timeout);
                if (!prefix_result) {
                    request_state_ = RequestState::Failed;
                    active_ = false;
                    conn_->fail_exchange(this);
                    conn_ = nullptr;
                    co_return std::unexpected(prefix_result.error());
                }
                auto body_result = co_await write_all(conn_->transport_.get(), buf, len, options_.write_timeout);
                if (!body_result) {
                    request_state_ = RequestState::Failed;
                    active_ = false;
                    conn_->fail_exchange(this);
                    conn_ = nullptr;
                    co_return std::unexpected(body_result.error());
                }
                auto suffix_result = co_await write_all(conn_->transport_.get(), kLineTerminator.data(),
                                                        kLineTerminator.size(), options_.write_timeout);
                if (!suffix_result) {
                    request_state_ = RequestState::Failed;
                    active_ = false;
                    conn_->fail_exchange(this);
                    conn_ = nullptr;
                    co_return std::unexpected(suffix_result.error());
                }
                body_sent_ += len;
            }

            if (end_stream) {
                auto final_result = co_await write_all(conn_->transport_.get(), kChunkedFinal.data(),
                                                       kChunkedFinal.size(), options_.write_timeout);
                if (!final_result) {
                    request_state_ = RequestState::Failed;
                    active_ = false;
                    conn_->fail_exchange(this);
                    conn_ = nullptr;
                    co_return std::unexpected(final_result.error());
                }
                request_state_ = RequestState::RequestDone;
            }
            co_return len;
        }
    }

    co_return std::unexpected(common::IoErr::Invalid);
}

fiber::async::Task<common::IoResult<void>> ClientHttp1Exchange::send_trailer(const HttpHeaders &trailers) noexcept {
    if (!active_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (!conn_ || !conn_->transport_ || !conn_->valid()) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (request_state_ == RequestState::Init) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (request_state_ == RequestState::RequestDone) {
        co_return std::unexpected(common::IoErr::Already);
    }
    if (request_state_ == RequestState::Failed || !body_spec_.is_chunked()) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (final_response_received_) {
        co_return std::unexpected(common::IoErr::Already);
    }

    auto estimated_size = estimate_trailer_bytes(trailers);
    if (!estimated_size) {
        co_return std::unexpected(estimated_size.error());
    }
    mem::IoBuf trailer_buf = mem::IoBuf::allocate(*estimated_size);
    if (!trailer_buf) {
        co_return std::unexpected(common::IoErr::NoMem);
    }
    auto encode_result = encode_chunked_trailer(trailer_buf, trailers);
    if (!encode_result) {
        co_return std::unexpected(encode_result.error());
    }

    auto write_result = co_await write_all(conn_->transport_.get(), trailer_buf.readable_data(), trailer_buf.readable(),
                                           options_.write_timeout);
    if (!write_result) {
        request_state_ = RequestState::Failed;
        active_ = false;
        conn_->fail_exchange(this);
        conn_ = nullptr;
        co_return std::unexpected(write_result.error());
    }

    request_state_ = RequestState::RequestDone;
    co_return common::IoResult<void>{};
}

fiber::async::Task<common::IoResult<const Http1ResponseHead *>> ClientHttp1Exchange::read_header() noexcept {
    if (!active_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (!conn_ || !conn_->transport_ || !conn_->valid() || pool_ == nullptr) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (request_state_ == RequestState::Init || request_state_ == RequestState::Failed) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (final_response_received_) {
        if (!response_headers_head_) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
        co_return &response_headers_head_->head;
    }

    response_trailers_.clear();
    response_body_parser_.reset();
    saw_connection_close_ = false;
    saw_connection_keep_alive_ = false;
    response_eof_delimited_ = false;

    auto *header_node = new (*pool_) ResponseHeaderNode(*pool_, event::EventLoop::current().io_buf_node_pool());
    if (!header_node) {
        co_return std::unexpected(common::IoErr::NoMem);
    }

    Http1HeaderParseBuffer response_header_buffer(response_header_buffer_options(options_));
    ResponseLineParser response_line_parser;
    HeaderLineParser response_header_parser;
    bool have_status_line = false;
    ResponseHeaderParseState header_parse_state{};

    auto fail_exchange = [&](common::IoErr err) -> common::IoResult<const Http1ResponseHead *> {
        header_node->~ResponseHeaderNode();
        ResponseHeaderNode::operator delete(header_node);
        request_state_ = RequestState::Failed;
        active_ = false;
        conn_->fail_exchange(this);
        conn_ = nullptr;
        return std::unexpected(err);
    };

    auto append_parsed_prefix = [&](Http1HeaderParseBuffer &buffer,
                                    mem::IoBufChain &owner_bufs) -> common::IoResult<void> {
        std::size_t consumed = static_cast<std::size_t>(buffer.buf().readable_data() - buffer.buf().data());
        if (consumed == 0) {
            return {};
        }
        auto retained = buffer.retain_prefix(consumed);
        if (!retained) {
            return std::unexpected(retained.error());
        }
        if (!owner_bufs.append(std::move(*retained))) {
            return std::unexpected(common::IoErr::NoMem);
        }
        return {};
    };

    if (pending_buf_) {
        auto init_result = response_header_buffer.ensure_init();
        if (!init_result) {
            co_return fail_exchange(init_result.error());
        }
        while (response_header_buffer.buf().writable() < pending_buf_.readable()) {
            if (!response_header_buffer.can_grow()) {
                co_return fail_exchange(common::IoErr::Invalid);
            }
            auto grow_result = response_header_buffer.grow();
            if (!grow_result) {
                co_return fail_exchange(grow_result.error());
            }
        }
        std::memcpy(response_header_buffer.buf().writable_data(), pending_buf_.readable_data(),
                    pending_buf_.readable());
        response_header_buffer.buf().commit(pending_buf_.readable());
        pending_buf_ = {};
    } else {
        auto init_result = response_header_buffer.ensure_init();
        if (!init_result) {
            co_return fail_exchange(init_result.error());
        }
    }

    auto read_more = [&]() -> fiber::async::Task<common::IoResult<void>> {
        auto read_result =
                co_await conn_->transport_->read_into(response_header_buffer.buf(), options_.response_header_timeout);
        if (!read_result) {
            co_return std::unexpected(read_result.error());
        }
        if (*read_result == 0) {
            co_return std::unexpected(common::IoErr::ConnReset);
        }
        co_return common::IoResult<void>{};
    };

    while (!have_status_line) {
        ParseCode code = response_line_parser.execute(&response_header_buffer.buf());
        if (code == ParseCode::Ok) {
            const auto &line = response_line_parser.state();
            header_node->head.version = to_http_version(line.http_version);
            header_node->head.status_code = line.status_code;
            if (line.reason_start && line.reason_end && line.reason_end >= line.reason_start) {
                header_node->head.reason =
                        std::string_view(reinterpret_cast<const char *>(line.reason_start),
                                         static_cast<std::size_t>(line.reason_end - line.reason_start));
            } else {
                header_node->head.reason = {};
            }
            have_status_line = true;
            break;
        }
        if (code != ParseCode::Again) {
            co_return fail_exchange(common::IoErr::Invalid);
        }
        if (response_header_buffer.buf().writable() == 0) {
            auto retain_result = append_parsed_prefix(response_header_buffer, header_node->owner_bufs);
            if (!retain_result) {
                co_return fail_exchange(retain_result.error());
            }
            if (!response_header_buffer.can_grow()) {
                co_return fail_exchange(common::IoErr::Invalid);
            }
            auto grow_result = response_header_buffer.grow_and_replace(response_line_parser);
            if (!grow_result) {
                co_return fail_exchange(grow_result.error());
            }
        }
        auto read_result = co_await read_more();
        if (!read_result) {
            co_return fail_exchange(read_result.error());
        }
    }

    for (;;) {
        ParseCode code = response_header_parser.execute(&response_header_buffer.buf());
        if (code == ParseCode::Ok) {
            const auto &line = response_header_parser.state();
            if (!line.header_name_start || !line.header_name_end || line.header_name_end < line.header_name_start) {
                co_return fail_exchange(common::IoErr::Invalid);
            }
            std::size_t name_len = static_cast<std::size_t>(line.header_name_end - line.header_name_start);
            std::string_view name(reinterpret_cast<const char *>(line.header_name_start), name_len);
            std::string_view value;
            if (line.header_start && line.header_end && line.header_end >= line.header_start) {
                value = std::string_view(reinterpret_cast<const char *>(line.header_start),
                                         static_cast<std::size_t>(line.header_end - line.header_start));
            }

            char *lowercase = static_cast<char *>(pool_->alloc(name_len));
            if (name_len != 0 && lowercase == nullptr) {
                co_return fail_exchange(common::IoErr::NoMem);
            }
            uint64_t hash = http_header_name_to_lowercase_and_hash(name, lowercase);
            HttpHeaders::HeaderField *field = header_node->head.headers.add_view(name, value, lowercase, hash);
            if (!field) {
                co_return fail_exchange(common::IoErr::NoMem);
            }
            if (auto *handler = response_header_handler_map().get(std::string_view(lowercase, name_len),
                                                                  static_cast<std::uint32_t>(hash))) {
                if (!(*handler)(header_parse_state, *field)) {
                    co_return fail_exchange(common::IoErr::Invalid);
                }
            }
            continue;
        }

        if (code == ParseCode::Again) {
            if (response_header_buffer.buf().writable() == 0) {
                auto retain_result = append_parsed_prefix(response_header_buffer, header_node->owner_bufs);
                if (!retain_result) {
                    co_return fail_exchange(retain_result.error());
                }
                if (!response_header_buffer.can_grow()) {
                    co_return fail_exchange(common::IoErr::Invalid);
                }
                auto grow_result = response_header_buffer.grow_and_replace(response_header_parser);
                if (!grow_result) {
                    co_return fail_exchange(grow_result.error());
                }
            }
            auto read_result = co_await read_more();
            if (!read_result) {
                co_return fail_exchange(read_result.error());
            }
            continue;
        }

        if (code != ParseCode::HeaderDone) {
            co_return fail_exchange(common::IoErr::Invalid);
        }

        auto retain_result = append_parsed_prefix(response_header_buffer, header_node->owner_bufs);
        if (!retain_result) {
            co_return fail_exchange(retain_result.error());
        }

        if (header_node->head.is_informational()) {
            if (response_header_buffer.buf().readable() > 0) {
                auto pending_header_result = response_header_buffer.retain_suffix();
                if (!pending_header_result) {
                    co_return fail_exchange(pending_header_result.error());
                }
                pending_buf_ = std::move(*pending_header_result);
            }
            header_node->next = response_headers_head_;
            response_headers_head_ = header_node;
            co_return &header_node->head;
        }

        final_response_received_ = true;
        saw_connection_close_ = header_parse_state.connection_close;
        saw_connection_keep_alive_ = header_parse_state.connection_keep_alive;
        bool allow_keepalive = !saw_connection_close_;
        if (!default_keepalive(header_node->head.version)) {
            allow_keepalive = allow_keepalive && saw_connection_keep_alive_;
        }

        if (response_has_no_body(request_method_, header_node->head.status_code)) {
            response_body_parser_.set_none();
            response_complete_ = true;
            keepalive_on_release_ = allow_keepalive;
        } else if (header_parse_state.chunked) {
            response_body_parser_.set_chunked();
            response_complete_ = false;
            keepalive_on_release_ = allow_keepalive;
        } else if (header_parse_state.content_length_set) {
            response_body_parser_.set_content_length(header_parse_state.content_length);
            response_complete_ = (header_parse_state.content_length == 0);
            keepalive_on_release_ = allow_keepalive;
        } else {
            response_body_parser_.set_none();
            response_complete_ = false;
            response_eof_delimited_ = true;
            keepalive_on_release_ = false;
        }

        if (response_header_buffer.buf().readable() > 0) {
            auto pending_body_result = response_header_buffer.retain_suffix();
            if (!pending_body_result) {
                co_return fail_exchange(pending_body_result.error());
            }
            pending_buf_ = std::move(*pending_body_result);
        }
        header_node->next = response_headers_head_;
        response_headers_head_ = header_node;
        co_return &header_node->head;
    }
}

fiber::async::Task<common::IoResult<mem::IoBufChain>> ClientHttp1Exchange::read_body(std::size_t max_bytes) noexcept {
    if (!active_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (!conn_ || !conn_->transport_ || !conn_->valid()) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    mem::IoBufChain out(event::EventLoop::current().io_buf_node_pool());
    if (!final_response_received_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (response_complete_) {
        out.mark_complete();
        co_return out;
    }
    if (response_body_parser_.type() == BodyParser::Type::None && !response_eof_delimited_) {
        response_complete_ = true;
        out.mark_complete();
        co_return out;
    }

    mem::IoBuf read_buf = std::move(pending_buf_);
    pending_buf_ = {};
    bool read_call_used_io = false;
    auto fail_exchange = [&](common::IoErr err) -> common::IoResult<mem::IoBufChain> {
        fail_active_exchange();
        return std::unexpected(err);
    };

    if (response_eof_delimited_) {
        if (max_bytes == 0) {
            co_return out;
        }
        if (read_buf.readable() == 0) {
            auto more = co_await read_more(read_buf, max_bytes, read_call_used_io);
            if (!more) {
                co_return fail_exchange(more.error());
            }
            if (*more == 0) {
                response_complete_ = true;
                out.mark_complete();
                co_return out;
            }
        }

        std::size_t take = std::min(max_bytes, read_buf.readable());
        auto take_result = take_prefix(read_buf, out, take);
        if (!take_result) {
            co_return fail_exchange(take_result.error());
        }
        auto stash_result = stash_pending_buf(read_buf);
        if (!stash_result) {
            co_return fail_exchange(stash_result.error());
        }
        co_return out;
    }

    if (response_body_parser_.type() == BodyParser::Type::ContentLength) {
        if (max_bytes == 0) {
            co_return out;
        }
        if (read_buf.readable() == 0) {
            auto more = co_await read_more(read_buf, std::min(max_bytes, response_body_parser_.remaining()),
                                           read_call_used_io);
            if (!more) {
                co_return fail_exchange(more.error());
            }
            if (*more == 0) {
                co_return fail_exchange(common::IoErr::ConnReset);
            }
        }

        std::size_t take = std::min({max_bytes, response_body_parser_.remaining(), read_buf.readable()});
        auto take_result = take_prefix(read_buf, out, take);
        if (!take_result) {
            co_return fail_exchange(take_result.error());
        }
        response_body_parser_.consume(take);
        if (response_body_parser_.done()) {
            response_complete_ = true;
            out.mark_complete();
        }

        auto stash_result = stash_pending_buf(read_buf);
        if (!stash_result) {
            co_return fail_exchange(stash_result.error());
        }
        co_return out;
    }

    if (response_body_parser_.type() != BodyParser::Type::Chunked) {
        co_return fail_exchange(common::IoErr::Invalid);
    }
    if (max_bytes == 0) {
        co_return out;
    }

    std::size_t remaining_budget = max_bytes;
    for (;;) {
        if (response_body_parser_.remaining() == 0) {
            auto parse_result =
                    co_await advance_chunked_body(read_buf, remaining_budget, !read_call_used_io, read_call_used_io);
            if (!parse_result) {
                co_return fail_exchange(parse_result.error());
            }
            if (*parse_result == ParseCode::BodyDone) {
                auto trailer_result = co_await read_response_trailers(read_buf);
                if (!trailer_result) {
                    co_return fail_exchange(trailer_result.error());
                }
                out.mark_complete();
                co_return out;
            }
            if (*parse_result == ParseCode::Again) {
                auto stash_result = stash_pending_buf(read_buf);
                if (!stash_result) {
                    co_return fail_exchange(stash_result.error());
                }
                co_return out;
            }
        }

        if (read_buf.readable() == 0) {
            if (read_call_used_io) {
                auto stash_result = stash_pending_buf(read_buf);
                if (!stash_result) {
                    co_return fail_exchange(stash_result.error());
                }
                co_return out;
            }
            auto more = co_await read_more(read_buf, std::min(remaining_budget, response_body_parser_.remaining()),
                                           read_call_used_io);
            if (!more) {
                co_return fail_exchange(more.error());
            }
            if (*more == 0) {
                co_return fail_exchange(common::IoErr::ConnReset);
            }
        }

        std::size_t take = std::min({remaining_budget, response_body_parser_.remaining(), read_buf.readable()});
        auto take_result = take_prefix(read_buf, out, take);
        if (!take_result) {
            co_return fail_exchange(take_result.error());
        }
        response_body_parser_.consume(take);
        remaining_budget -= take;

        if (remaining_budget == 0 || response_body_parser_.remaining() > 0) {
            auto stash_result = stash_pending_buf(read_buf);
            if (!stash_result) {
                co_return fail_exchange(stash_result.error());
            }
            co_return out;
        }
    }
}

fiber::async::Task<common::IoResult<void>> ClientHttp1Exchange::discard_response_body() noexcept {
    if (!active_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    for (;;) {
        auto result = co_await read_body(4096);
        if (!result) {
            co_return std::unexpected(result.error());
        }
        if (result->complete()) {
            break;
        }
    }
    co_return common::IoResult<void>{};
}

} // namespace fiber::http
