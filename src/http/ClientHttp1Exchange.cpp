#include "ClientHttp1Exchange.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <limits>
#include <system_error>
#include <utility>

#include "HttpHeaderHash.h"
#include "Http1ClientConnection.h"
#include "HeaderMap.h"
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

bool is_informational_status(int status_code) noexcept { return status_code >= 100 && status_code < 200; }

bool response_has_no_body(HttpMethod request_method, int status_code) noexcept {
    return request_method == HttpMethod::Head || is_informational_status(status_code) || status_code == 204 ||
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
    switch (head.body_mode) {
        case Http1RequestBodyMode::None:
            break;
        case Http1RequestBodyMode::ContentLength:
            if (head.content_length > static_cast<std::size_t>(std::numeric_limits<std::uint64_t>::max())) {
                return std::unexpected(common::IoErr::Invalid);
            }
            total += kContentLengthPrefix.size() + kMaxContentLengthDigits + kLineTerminator.size();
            break;
        case Http1RequestBodyMode::Chunked:
            total += kChunkedHeader.size();
            break;
    }

    if (head.headers) {
        for (const auto &field : *head.headers) {
            if (field.name_len >
                std::numeric_limits<std::size_t>::max() - total - kHeaderNameValueSep.size() - field.value_len -
                        kLineTerminator.size()) {
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

    switch (head.body_mode) {
        case Http1RequestBodyMode::None:
            break;
        case Http1RequestBodyMode::ContentLength:
            append_bytes(ptr, kContentLengthPrefix);
            ptr += append_decimal(ptr, head.content_length);
            append_bytes(ptr, kLineTerminator);
            break;
        case Http1RequestBodyMode::Chunked:
            append_bytes(ptr, kChunkedHeader);
            break;
    }

    if (head.headers) {
        for (const auto &field : *head.headers) {
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
    for (const auto &field : trailers) {
        if (field.name_len >
            std::numeric_limits<std::size_t>::max() - total - kHeaderNameValueSep.size() - field.value_len -
                    kLineTerminator.size()) {
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
    for (const auto &field : trailers) {
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

ClientHttp1Exchange::ClientHttp1Exchange(Http1ClientConnection &conn,
                                         mem::BufPool &pool,
                                         Http1ClientExchangeOptions options) noexcept
    : conn_(&conn),
      pool_(&pool),
      options_(std::move(options)),
      response_head_(pool),
      response_trailers_(pool),
      response_header_buffer_(response_header_buffer_options(options_)) {
    active_ = conn.acquire_exchange(this);
}

ClientHttp1Exchange::~ClientHttp1Exchange() {
    if (!conn_ || !active_) {
        return;
    }
    if (done()) {
        conn_->release_exchange(this, keepalive_on_release_);
    } else {
        conn_->fail_exchange(this);
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
    if (head.body_mode == Http1RequestBodyMode::None && !end_stream) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (head.body_mode == Http1RequestBodyMode::ContentLength && end_stream && head.content_length != 0) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (head.body_mode == Http1RequestBodyMode::Chunked && end_stream) {
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

    auto write_result = co_await write_all(conn_->transport_.get(),
                                           header_buf.readable_data(),
                                           header_buf.readable(),
                                           options_.write_timeout);
    if (!write_result) {
        request_state_ = RequestState::Failed;
        active_ = false;
        conn_->fail_exchange(this);
        conn_ = nullptr;
        co_return std::unexpected(write_result.error());
    }

    body_mode_ = head.body_mode;
    content_length_ = head.content_length;
    body_sent_ = 0;
    request_method_ = head.method;
    final_response_received_ = false;
    response_complete_ = false;
    keepalive_on_release_ = false;
    saw_connection_close_ = false;
    saw_connection_keep_alive_ = false;
    response_eof_delimited_ = false;
    response_header_buffer_.reset();
    header_owner_buf_ = {};
    pending_header_buf_ = {};
    pending_body_buf_ = {};
    response_head_.headers.clear();
    response_trailers_.clear();
    response_head_.version = HttpVersion::HTTP_1_1;
    response_head_.status_code = 0;
    response_head_.reason = {};
    response_body_parser_.reset();
    request_state_ = end_stream || head.body_mode == Http1RequestBodyMode::None ? RequestState::RequestDone
                                                                                 : RequestState::SendingBody;
    co_return common::IoResult<void>{};
}

fiber::async::Task<common::IoResult<std::size_t>> ClientHttp1Exchange::write_body(BodyChunk chunk) noexcept {
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

    const std::size_t body_bytes = chunk.data_chain.readable_bytes();
    switch (body_mode_) {
        case Http1RequestBodyMode::None:
            co_return std::unexpected(common::IoErr::Invalid);
        case Http1RequestBodyMode::ContentLength: {
            if (body_bytes > content_length_ - body_sent_) {
                co_return std::unexpected(common::IoErr::Invalid);
            }
            if (chunk.last && body_sent_ + body_bytes != content_length_) {
                co_return std::unexpected(common::IoErr::Invalid);
            }
            auto write_result = co_await write_all(conn_->transport_.get(), chunk.data_chain, options_.write_timeout);
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
        case Http1RequestBodyMode::Chunked: {
            if (body_bytes == 0 && !chunk.last) {
                co_return static_cast<std::size_t>(0);
            }

            if (body_bytes != 0) {
                std::array<char, kMaxChunkSizeHexDigits + 2> prefix{};
                char *prefix_ptr = prefix.data();
                prefix_ptr += append_hex(prefix_ptr, body_bytes);
                *prefix_ptr++ = '\r';
                *prefix_ptr++ = '\n';

                auto prefix_result = co_await write_all(conn_->transport_.get(),
                                                        prefix.data(),
                                                        static_cast<std::size_t>(prefix_ptr - prefix.data()),
                                                        options_.write_timeout);
                if (!prefix_result) {
                    request_state_ = RequestState::Failed;
                    active_ = false;
                    conn_->fail_exchange(this);
                    conn_ = nullptr;
                    co_return std::unexpected(prefix_result.error());
                }
                auto body_result =
                    co_await write_all(conn_->transport_.get(), chunk.data_chain, options_.write_timeout);
                if (!body_result) {
                    request_state_ = RequestState::Failed;
                    active_ = false;
                    conn_->fail_exchange(this);
                    conn_ = nullptr;
                    co_return std::unexpected(body_result.error());
                }
                auto suffix_result = co_await write_all(conn_->transport_.get(),
                                                        kLineTerminator.data(),
                                                        kLineTerminator.size(),
                                                        options_.write_timeout);
                if (!suffix_result) {
                    request_state_ = RequestState::Failed;
                    active_ = false;
                    conn_->fail_exchange(this);
                    conn_ = nullptr;
                    co_return std::unexpected(suffix_result.error());
                }
                body_sent_ += body_bytes;
            }

            if (chunk.last) {
                auto final_result = co_await write_all(conn_->transport_.get(),
                                                       kChunkedFinal.data(),
                                                       kChunkedFinal.size(),
                                                       options_.write_timeout);
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

fiber::async::Task<common::IoResult<std::size_t>> ClientHttp1Exchange::write_body(const std::uint8_t *buf,
                                                                                   std::size_t len,
                                                                                   bool end_stream) noexcept {
    if (!active_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (len != 0 && buf == nullptr) {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    BodyChunk chunk;
    chunk.last = end_stream;
    if (len != 0) {
        mem::IoBuf owned = mem::IoBuf::allocate(len);
        if (!owned) {
            co_return std::unexpected(common::IoErr::NoMem);
        }
        std::memcpy(owned.writable_data(), buf, len);
        owned.commit(len);
        if (!chunk.data_chain.append(std::move(owned))) {
            co_return std::unexpected(common::IoErr::NoMem);
        }
    }
    co_return co_await write_body(std::move(chunk));
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
    if (request_state_ == RequestState::Failed || body_mode_ != Http1RequestBodyMode::Chunked) {
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

    auto write_result = co_await write_all(conn_->transport_.get(),
                                           trailer_buf.readable_data(),
                                           trailer_buf.readable(),
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
        co_return &response_head_;
    }

    response_head_.headers.clear();
    response_trailers_.clear();
    response_head_.reason = {};
    response_line_parser_.reset();
    response_header_parser_.reset();
    response_body_parser_.reset();
    header_owner_buf_ = {};
    pending_body_buf_ = {};
    saw_connection_close_ = false;
    saw_connection_keep_alive_ = false;
    response_eof_delimited_ = false;

    bool have_status_line = false;
    ResponseHeaderParseState header_parse_state{};

    auto fail_exchange = [&](common::IoErr err) -> common::IoResult<const Http1ResponseHead *> {
        request_state_ = RequestState::Failed;
        active_ = false;
        conn_->fail_exchange(this);
        conn_ = nullptr;
        return std::unexpected(err);
    };

    response_header_buffer_.reset();
    if (pending_header_buf_) {
        auto init_result = response_header_buffer_.ensure_init();
        if (!init_result) {
            co_return fail_exchange(init_result.error());
        }
        while (response_header_buffer_.buf().writable() < pending_header_buf_.readable()) {
            if (!response_header_buffer_.can_grow()) {
                co_return fail_exchange(common::IoErr::Invalid);
            }
            auto grow_result = response_header_buffer_.grow();
            if (!grow_result) {
                co_return fail_exchange(grow_result.error());
            }
        }
        std::memcpy(response_header_buffer_.buf().writable_data(),
                    pending_header_buf_.readable_data(),
                    pending_header_buf_.readable());
        response_header_buffer_.buf().commit(pending_header_buf_.readable());
        pending_header_buf_ = {};
    } else {
        auto init_result = response_header_buffer_.ensure_init();
        if (!init_result) {
            co_return fail_exchange(init_result.error());
        }
    }

    auto read_more = [&]() -> fiber::async::Task<common::IoResult<void>> {
        auto read_result =
            co_await conn_->transport_->read_into(response_header_buffer_.buf(), options_.response_header_timeout);
        if (!read_result) {
            co_return std::unexpected(read_result.error());
        }
        if (*read_result == 0) {
            co_return std::unexpected(common::IoErr::ConnReset);
        }
        co_return common::IoResult<void>{};
    };

    while (!have_status_line) {
        ParseCode code = response_line_parser_.execute(&response_header_buffer_.buf());
        if (code == ParseCode::Ok) {
            const auto &line = response_line_parser_.state();
            response_head_.version = to_http_version(line.http_version);
            response_head_.status_code = line.status_code;
            if (line.reason_start && line.reason_end && line.reason_end >= line.reason_start) {
                response_head_.reason = std::string_view(reinterpret_cast<const char *>(line.reason_start),
                                                        static_cast<std::size_t>(line.reason_end - line.reason_start));
            } else {
                response_head_.reason = {};
            }
            have_status_line = true;
            break;
        }
        if (code != ParseCode::Again) {
            co_return fail_exchange(common::IoErr::Invalid);
        }
        if (response_header_buffer_.buf().writable() == 0) {
            if (!response_header_buffer_.can_grow()) {
                co_return fail_exchange(common::IoErr::Invalid);
            }
            auto grow_result = response_header_buffer_.grow_and_replace(response_line_parser_);
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
        ParseCode code = response_header_parser_.execute(&response_header_buffer_.buf());
        if (code == ParseCode::Ok) {
            const auto &line = response_header_parser_.state();
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
            HttpHeaders::HeaderField *field = response_head_.headers.add_view(name, value, lowercase, hash);
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
            if (response_header_buffer_.buf().writable() == 0) {
                if (!response_header_buffer_.can_grow()) {
                    co_return fail_exchange(common::IoErr::Invalid);
                }
                auto grow_result = response_header_buffer_.grow_and_replace(response_header_parser_);
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

        std::size_t header_bytes =
            static_cast<std::size_t>(response_header_buffer_.buf().readable_data() - response_header_buffer_.buf().data());
        auto header_owner_result = response_header_buffer_.retain_prefix(header_bytes);
        if (!header_owner_result) {
            co_return fail_exchange(header_owner_result.error());
        }
        header_owner_buf_ = std::move(*header_owner_result);

        if (is_informational_status(response_head_.status_code)) {
            if (response_header_buffer_.buf().readable() > 0) {
                auto pending_header_result = response_header_buffer_.retain_suffix();
                if (!pending_header_result) {
                    co_return fail_exchange(pending_header_result.error());
                }
                pending_header_buf_ = std::move(*pending_header_result);
            }
            response_header_buffer_.reset();
            co_return &response_head_;
        }

        final_response_received_ = true;
        saw_connection_close_ = header_parse_state.connection_close;
        saw_connection_keep_alive_ = header_parse_state.connection_keep_alive;
        bool allow_keepalive = !saw_connection_close_;
        if (!default_keepalive(response_head_.version)) {
            allow_keepalive = allow_keepalive && saw_connection_keep_alive_;
        }

        if (response_has_no_body(request_method_, response_head_.status_code)) {
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

        if (response_header_buffer_.buf().readable() > 0) {
            auto pending_body_result = response_header_buffer_.retain_suffix();
            if (!pending_body_result) {
                co_return fail_exchange(pending_body_result.error());
            }
            pending_body_buf_ = std::move(*pending_body_result);
        }
        response_header_buffer_.reset();
        co_return &response_head_;
    }
}

fiber::async::Task<common::IoResult<BodyChunk>> ClientHttp1Exchange::read_body(std::size_t) noexcept {
    if (!active_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return std::unexpected(common::IoErr::NotSupported);
}

fiber::async::Task<common::IoResult<void>> ClientHttp1Exchange::discard_response_body() noexcept {
    if (!active_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return std::unexpected(common::IoErr::NotSupported);
}

} // namespace fiber::http
