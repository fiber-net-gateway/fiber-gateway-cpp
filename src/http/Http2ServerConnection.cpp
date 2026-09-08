#include <fiber/http/Http2ServerConnection.h>

#include <utility>

#include <fiber/common/Assert.h>

namespace fiber::http {

Http2ServerConnection::Http2ServerConnection(event::EventLoop &loop, Http2Connection::Options options,
                                             ServerRequestFactory &request_factory) noexcept :
    loop_(&loop), request_factory_(&request_factory), conn_(std::move(options), this, connection_ops()),
    close_gate_(loop, conn_) {}

const Http2Connection::Ops &Http2ServerConnection::connection_ops() noexcept {
    static const Http2Connection::Ops ops{&create_peer_stream, &on_state_change, nullptr};
    return ops;
}

Http2Stream::Lease Http2ServerConnection::create_peer_stream(void *ctx, std::uint32_t id,
                                                             Http2Connection &conn) noexcept {
    return static_cast<Http2ServerConnection *>(ctx)->request_factory_->create_peer_stream(id, conn);
}

void Http2ServerConnection::on_state_change(void *ctx, Http2Connection &connection) noexcept {
    if (connection.state() == Http2Connection::State::Closed) {
        static_cast<Http2ServerConnection *>(ctx)->close_gate_.on_connection_closed();
    }
}

Http2ServerConnection::~Http2ServerConnection() { FIBER_ASSERT(!worker_hook_.linked()); }

common::IoErr Http2ServerConnection::start(std::unique_ptr<HttpTransport> transport) noexcept {
    if (!transport || &transport->loop() != loop_) {
        return common::IoErr::Invalid;
    }
    return conn_.start(std::move(transport));
}

fiber::async::Task<Http2Connection::CloseResult> Http2ServerConnection::wait_closed() noexcept {
    co_return co_await close_gate_.join();
}

void Http2ServerConnection::request_drain() noexcept { conn_.graceful_shutdown(); }

void Http2ServerConnection::request_shutdown() noexcept { conn_.shutdown(common::IoErr::Canceled); }

} // namespace fiber::http
