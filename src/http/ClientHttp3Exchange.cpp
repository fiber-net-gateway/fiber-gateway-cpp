#include <fiber/http/ClientHttp3Exchange.h>

#include <cstring>
#include <expected>
#include <utility>

#include <fiber/common/Assert.h>
#include <fiber/http/Http3ClientConnection.h>
#include <fiber/http/Http3Connection.h>
#include "http/ClientHttp3Request.h"

namespace fiber::http {

ClientHttp3Exchange::ClientHttp3Exchange(Http3Connection &conn, mem::BufPool &pool) noexcept :
    conn_(&conn), pool_(&pool) {}

ClientHttp3Exchange::ClientHttp3Exchange(Http3ClientConnection &conn, mem::BufPool &pool) noexcept :
    ClientHttp3Exchange(conn.http3(), pool) {}

ClientHttp3Exchange::ClientHttp3Exchange(ClientHttp3Exchange &&other) noexcept :
    conn_(other.conn_), pool_(other.pool_), stream_(std::move(other.stream_)) {
    other.conn_ = nullptr;
    other.pool_ = nullptr;
}

ClientHttp3Exchange &ClientHttp3Exchange::operator=(ClientHttp3Exchange &&other) noexcept {
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

async::Task<common::IoResult<void>> ClientHttp3Exchange::send_header(const ClientRequestHead &head, bool end_stream,
                                                                     std::chrono::milliseconds timeout) noexcept {
    auto opened = co_await ensure_request_opened(timeout);
    if (!opened) {
        co_return std::unexpected(opened.error());
    }
    co_return co_await (*opened)->send_request_header(head, end_stream, timeout);
}

async::Task<common::IoResult<std::size_t>> ClientHttp3Exchange::write_all(mem::IoBufChain chunk,
                                                                          std::chrono::milliseconds timeout) noexcept {
    ClientHttp3Request *req = request();
    if (req == nullptr) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return co_await req->write_all(std::move(chunk), timeout);
}

async::Task<common::IoResult<std::size_t>> ClientHttp3Exchange::write_all(const std::uint8_t *buf, std::size_t len,
                                                                          bool end_stream,
                                                                          std::chrono::milliseconds timeout) noexcept {
    if (len != 0 && buf == nullptr) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    ClientHttp3Request *req = request();
    if (req == nullptr) {
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
    co_return co_await req->write_all(std::move(chunk), timeout);
}

async::Task<common::IoResult<std::size_t>> ClientHttp3Exchange::write(mem::IoBufChain &chunk,
                                                                      std::chrono::milliseconds timeout) noexcept {
    ClientHttp3Request *req = request();
    if (req == nullptr) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return co_await req->write(chunk, timeout);
}

async::Task<common::IoResult<std::size_t>> ClientHttp3Exchange::write(const std::uint8_t *buf, std::size_t len,
                                                                      bool end_stream,
                                                                      std::chrono::milliseconds timeout) noexcept {
    if (len != 0 && buf == nullptr) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    ClientHttp3Request *req = request();
    if (req == nullptr) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return co_await req->write(buf, len, end_stream, timeout);
}

async::Task<common::IoResult<void>> ClientHttp3Exchange::send_trailer(const HttpHeaders &headers,
                                                                      std::chrono::milliseconds timeout) noexcept {
    ClientHttp3Request *req = request();
    if (req == nullptr) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return co_await req->write_trailer(headers, timeout);
}

async::Task<common::IoResult<const ClientResponseHead *>>
ClientHttp3Exchange::read_header(std::chrono::milliseconds timeout) noexcept {
    ClientHttp3Request *req = request();
    if (req == nullptr) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return co_await req->read_header(timeout);
}

async::Task<common::IoResult<mem::IoBufChain>>
ClientHttp3Exchange::read_body(std::size_t max_bytes, std::chrono::milliseconds timeout) noexcept {
    ClientHttp3Request *req = request();
    if (req == nullptr) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return co_await req->read_body(max_bytes, timeout);
}

common::IoResult<void> ClientHttp3Exchange::abort(common::IoErr reason) noexcept {
    ClientHttp3Request *req = request();
    if (req == nullptr) {
        conn_ = nullptr;
        return std::unexpected(common::IoErr::Invalid);
    }
    return req->abort(reason);
}

void ClientHttp3Exchange::cancel(common::IoErr reason) noexcept { (void) abort(reason); }

Http3ExtendedConnectSupport ClientHttp3Exchange::extended_connect_support() const noexcept {
    const ClientHttp3Request *req = request();
    if (req != nullptr) {
        return req->extended_connect_support();
    }
    if (conn_ == nullptr || !conn_->peer_settings_received()) {
        return Http3ExtendedConnectSupport::Unknown;
    }
    return conn_->peer_settings().enable_connect_protocol ? Http3ExtendedConnectSupport::Enabled
                                                          : Http3ExtendedConnectSupport::Disabled;
}

Http3RequestOutcome ClientHttp3Exchange::outcome() const noexcept {
    const ClientHttp3Request *req = request();
    return req == nullptr ? Http3RequestOutcome::NotSent : req->outcome();
}

std::uint64_t ClientHttp3Exchange::stream_id() const noexcept {
    return !stream_ ? quic::kQuicUnassignedStreamId : stream_->stream_id();
}

async::Task<common::IoResult<ClientHttp3Request *>>
ClientHttp3Exchange::ensure_request_opened(std::chrono::milliseconds timeout) noexcept {
    if (stream_) {
        ClientHttp3Request *req = request();
        co_return req == nullptr ? common::IoResult<ClientHttp3Request *>(std::unexpected(common::IoErr::Invalid))
                                 : common::IoResult<ClientHttp3Request *>(req);
    }
    if (conn_ == nullptr || pool_ == nullptr || !conn_->accepting_requests()) {
        co_return std::unexpected(conn_ == nullptr || pool_ == nullptr ? common::IoErr::Invalid
                                                                       : common::IoErr::Canceled);
    }

    quic::QuicStream::Lease owned = ClientHttp3Request::create(*conn_, *pool_);
    if (!owned) {
        co_return std::unexpected(common::IoErr::NoMem);
    }
    auto attached =
            co_await conn_->quic().attach_local_stream(std::move(owned), quic::QuicStreamType::Bidirectional, timeout);
    if (!attached) {
        co_return std::unexpected(attached.error());
    }

    stream_ = (*attached)->lease();
    ClientHttp3Request *req = request();
    if (req == nullptr) {
        stream_.reset();
        co_return std::unexpected(common::IoErr::Invalid);
    }
    auto registered = req->register_attached();
    if (!registered) {
        (void) req->abort(registered.error());
        stream_.reset();
        co_return std::unexpected(registered.error());
    }
    co_return req;
}

ClientHttp3Request *ClientHttp3Exchange::request() noexcept {
    return stream_ ? ClientHttp3Request::from_stream(*stream_) : nullptr;
}

const ClientHttp3Request *ClientHttp3Exchange::request() const noexcept {
    return stream_ ? ClientHttp3Request::from_stream(*stream_) : nullptr;
}

} // namespace fiber::http
