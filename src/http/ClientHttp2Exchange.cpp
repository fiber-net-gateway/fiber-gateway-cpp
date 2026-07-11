#include "ClientHttp2Exchange.h"

#include <cstring>
#include <new>
#include <utility>

#include "../common/Assert.h"
#include "ClientHttp2Request.h"
#include "Http2ClientConnection.h"
#include "Http2Connection.h"

namespace fiber::http {

ClientHttp2Exchange::ClientHttp2Exchange(Http2Connection &conn, mem::BufPool &pool) noexcept :
    conn_(&conn), pool_(&pool) {}

ClientHttp2Exchange::ClientHttp2Exchange(Http2ClientConnection &conn, mem::BufPool &pool) noexcept :
    ClientHttp2Exchange(conn.http2(), pool) {}

ClientHttp2Exchange::ClientHttp2Exchange(Http2Stream::Lease stream, mem::BufPool &pool) noexcept :
    pool_(&pool), stream_(std::move(stream)) {}

ClientHttp2Exchange::ClientHttp2Exchange(ClientHttp2Exchange &&other) noexcept :
    conn_(other.conn_), pool_(other.pool_), stream_(std::move(other.stream_)) {
    other.conn_ = nullptr;
    other.pool_ = nullptr;
}

ClientHttp2Exchange &ClientHttp2Exchange::operator=(ClientHttp2Exchange &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    conn_ = other.conn_;
    pool_ = other.pool_;
    stream_ = std::move(other.stream_);
    other.conn_ = nullptr;
    other.pool_ = nullptr;
    return *this;
}

fiber::async::Task<common::IoResult<void>>
ClientHttp2Exchange::send_request_header(const Http2RequestHead &head, bool end_stream,
                                         std::chrono::milliseconds timeout) noexcept {
    auto request_result = ensure_request_opened();
    if (!request_result) {
        co_return std::unexpected(request_result.error());
    }
    ClientHttp2Request *req = *request_result;
    FIBER_ASSERT(req != nullptr);
    co_return co_await req->send_request_header(head, end_stream, timeout);
}

fiber::async::Task<common::IoResult<size_t>>
ClientHttp2Exchange::write_body(mem::IoBufChain chunk, std::chrono::milliseconds timeout) noexcept {
    if (!stream_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    ClientHttp2Request *req = request();
    if (!req) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return co_await req->write_body(std::move(chunk), timeout);
}

fiber::async::Task<common::IoResult<size_t>>
ClientHttp2Exchange::write_body(const std::uint8_t *buf, std::size_t len, bool end_stream,
                                std::chrono::milliseconds timeout) noexcept {
    if (len != 0 && buf == nullptr) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (!stream_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    ClientHttp2Request *req = request();
    if (!req) {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    mem::IoBufChain chunk(req->node_pool());
    if (end_stream) {
        chunk.mark_complete();
    }
    if (len != 0) {
        mem::IoBuf owned = mem::IoBuf::allocate(len);
        if (!owned) {
            co_return std::unexpected(common::IoErr::NoMem);
        }
        std::memcpy(owned.writable_data(), buf, len);
        owned.commit(len);
        if (!chunk.append(std::move(owned))) {
            co_return std::unexpected(common::IoErr::NoMem);
        }
    }

    co_return co_await req->write_body(std::move(chunk), timeout);
}

fiber::async::Task<common::IoResult<void>>
ClientHttp2Exchange::write_trailer(const HttpHeaders &headers, std::chrono::milliseconds timeout) noexcept {
    if (!stream_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    ClientHttp2Request *req = request();
    if (!req) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return co_await req->write_trailer(headers, timeout);
}

fiber::async::Task<common::IoResult<const Http2ResponseHead *>>
ClientHttp2Exchange::read_header(std::chrono::milliseconds timeout) noexcept {
    if (!stream_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    ClientHttp2Request *req = request();
    if (!req) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return co_await req->read_header(timeout);
}

fiber::async::Task<common::IoResult<mem::IoBufChain>>
ClientHttp2Exchange::read_body(std::size_t max_bytes, std::chrono::milliseconds timeout) noexcept {
    if (!stream_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    ClientHttp2Request *req = request();
    if (!req) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return co_await req->read_body(max_bytes, timeout);
}

void ClientHttp2Exchange::cancel(common::IoErr reason) noexcept {
    if (!stream_) {
        conn_ = nullptr;
        return;
    }
    stream_->close(reason);
}

common::IoResult<ClientHttp2Request *> ClientHttp2Exchange::ensure_request_opened() noexcept {
    if (stream_) {
        ClientHttp2Request *req = request();
        if (!req) {
            return std::unexpected(common::IoErr::Invalid);
        }
        return req;
    }
    if (!conn_) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (!pool_) {
        return std::unexpected(common::IoErr::Invalid);
    }

    ClientHttp2Request *req = ClientHttp2Request::create(*conn_, *pool_);
    if (!req) {
        return std::unexpected(common::IoErr::NoMem);
    }
    auto attach_result = conn_->attach_local_stream(req->stream());
    if (!attach_result) {
        delete req;
        return std::unexpected(attach_result.error());
    }
    stream_ = std::move(*attach_result);
    req = request();
    if (!req) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return req;
}

ClientHttp2Request *ClientHttp2Exchange::request() noexcept {
    return stream_ ? static_cast<ClientHttp2Request *>(stream_->owner()) : nullptr;
}

const ClientHttp2Request *ClientHttp2Exchange::request() const noexcept {
    return stream_ ? static_cast<const ClientHttp2Request *>(stream_->owner()) : nullptr;
}

} // namespace fiber::http
