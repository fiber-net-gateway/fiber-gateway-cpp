#include "ServerHttp3Request.h"

#include <expected>
#include <new>
#include <utility>

#include "../async/Spawn.h"
#include "../event/EventLoop.h"
#include "Http3Connection.h"

namespace fiber::http {

namespace {

constexpr std::size_t kHttp3RequestReadChunkSize = 4096;

} // namespace

ServerHttp3Request::ServerHttp3Request(Http3Connection &conn, const HttpServerOptions &http_options,
                                       const HttpHandler &handler) noexcept :
    quic_lease_(conn.quic().lease()), stream_(this, &ServerHttp3Request::destroy_owner),
    exchange_(conn.quic().recv_extent_pool(), http_options), handler_(&handler) {}

quic::QuicStream::Lease ServerHttp3Request::create(std::uint64_t stream_id, Http3Connection &conn,
                                                   const HttpServerOptions &http_options,
                                                   const HttpHandler &handler) noexcept {
    (void) stream_id;
    auto *request = new (std::nothrow) ServerHttp3Request(conn, http_options, handler);
    if (request == nullptr) {
        return {};
    }
    return quic::QuicStream::Lease::adopt(&request->stream_);
}

ServerHttp3Request *ServerHttp3Request::from_stream(quic::QuicStream &stream) noexcept {
    if (stream.destroy_callback() != &ServerHttp3Request::destroy_owner) {
        return nullptr;
    }
    auto *request = static_cast<ServerHttp3Request *>(stream.owner());
    if (request == nullptr || &request->stream_ != &stream) {
        return nullptr;
    }
    return request;
}

const ServerHttp3Request *ServerHttp3Request::from_stream(const quic::QuicStream &stream) noexcept {
    if (stream.destroy_callback() != &ServerHttp3Request::destroy_owner) {
        return nullptr;
    }
    auto *request = static_cast<const ServerHttp3Request *>(stream.owner());
    if (request == nullptr || &request->stream_ != &stream) {
        return nullptr;
    }
    return request;
}

void ServerHttp3Request::start_read_loop(event::EventLoop &loop) noexcept {
    if (read_loop_started_) {
        return;
    }
    read_loop_started_ = true;
    async::spawn(loop, [this, lease = stream_.lease()]() mutable { return run_read_loop(std::move(lease)); });
}

void ServerHttp3Request::destroy_owner(void *owner, quic::QuicStream &) noexcept {
    delete static_cast<ServerHttp3Request *>(owner);
}

async::DetachedTask ServerHttp3Request::run_read_loop(quic::QuicStream::Lease lease) noexcept {
    if (!lease) {
        co_return;
    }

    mem::IoBufChain input;

    for (;;) {
        auto read = co_await stream_.read(kHttp3RequestReadChunkSize, input);
        if (!read) {
            co_return;
        }
        const bool complete = input.complete();
        input.clear();
        if (complete) {
            co_return;
        }
    }
}

async::Task<common::IoResult<mem::IoBufChain>> ServerHttp3Request::read_body(HttpExchange &, std::size_t) noexcept {
    co_return std::unexpected(common::IoErr::NotSupported);
}

async::Task<common::IoResult<void>> ServerHttp3Request::send_header(HttpExchange &, const OutgoingHeaderBlockView &) {
    co_return std::unexpected(common::IoErr::NotSupported);
}

async::Task<common::IoResult<std::size_t>> ServerHttp3Request::write_body(HttpExchange &, mem::IoBufChain) noexcept {
    co_return std::unexpected(common::IoErr::NotSupported);
}

async::Task<common::IoResult<std::size_t>> ServerHttp3Request::write_body(HttpExchange &, const std::uint8_t *,
                                                                          std::size_t, bool) noexcept {
    co_return std::unexpected(common::IoErr::NotSupported);
}

} // namespace fiber::http
