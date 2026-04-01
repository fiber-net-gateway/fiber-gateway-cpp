#include "ClientHttp1Exchange.h"

#include <array>
#include <charconv>
#include <cstring>
#include <limits>
#include <system_error>
#include <utility>

#include "Http1ClientConnection.h"
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

ClientHttp1Exchange::ClientHttp1Exchange(Http1ClientConnection &conn, Http1ClientExchangeOptions options) noexcept
    : conn_(&conn), options_(std::move(options)), response_head_(pool_), response_trailers_(pool_) {
    active_ = conn.acquire_exchange(this);
}

ClientHttp1Exchange::~ClientHttp1Exchange() {
    if (!conn_ || !active_) {
        return;
    }
    if (done()) {
        conn_->release_exchange(this, true);
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
    co_return std::unexpected(common::IoErr::NotSupported);
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
