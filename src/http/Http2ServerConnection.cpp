#include <fiber/http/Http2ServerConnection.h>

#include <utility>

#include <fiber/common/Assert.h>

namespace fiber::http {

Http2ServerConnection::Http2ServerConnection(event::EventLoop &loop, Http2Connection::Options options,
                                             ServerRequestFactory &request_factory,
                                             std::chrono::milliseconds idle_timeout) noexcept :
    loop_(&loop), request_factory_(&request_factory), conn_(std::move(options), this, connection_ops()),
    close_gate_(loop, conn_), idle_timeout_(idle_timeout) {
    FIBER_ASSERT(idle_timeout_ >= std::chrono::milliseconds::zero());
}

const Http2Connection::Ops &Http2ServerConnection::connection_ops() noexcept {
    static const Http2Connection::Ops ops{&create_peer_stream, &on_state_change, &on_capacity_change};
    return ops;
}

Http2Stream::Lease Http2ServerConnection::create_peer_stream(void *ctx, std::uint32_t id,
                                                             Http2Connection &conn) noexcept {
    return static_cast<Http2ServerConnection *>(ctx)->request_factory_->create_peer_stream(id, conn);
}

void Http2ServerConnection::on_state_change(void *ctx, Http2Connection &connection) noexcept {
    auto &self = *static_cast<Http2ServerConnection *>(ctx);
    self.sync_idle_timer();
    if (connection.state() == Http2Connection::State::Closed) {
        self.close_gate_.on_connection_closed();
    }
}

void Http2ServerConnection::on_capacity_change(void *ctx, Http2Connection &) noexcept {
    static_cast<Http2ServerConnection *>(ctx)->sync_idle_timer();
}

void Http2ServerConnection::sync_idle_timer() noexcept {
    if (idle_timeout_ == std::chrono::milliseconds::max() || conn_.state() != Http2Connection::State::Running ||
        conn_.has_active_streams()) {
        cancel_idle_timer();
        return;
    }
    // Capacity notifications also cover SETTINGS. Preserve an existing idle
    // deadline until a stream is actually attached.
    if (idle_timer_entry_.is_in_heap()) {
        return;
    }
    loop_->post_at<Http2ServerConnection, &Http2ServerConnection::idle_timer_entry_,
                   &Http2ServerConnection::on_idle_timer>(event::EventLoop::current().now() + idle_timeout_, *this);
}

void Http2ServerConnection::cancel_idle_timer() noexcept {
    if (idle_timer_entry_.is_in_heap()) {
        loop_->cancel<Http2ServerConnection, &Http2ServerConnection::idle_timer_entry_>(*this);
    }
}

void Http2ServerConnection::on_idle_timer(Http2ServerConnection *connection) noexcept {
    if (connection->conn_.state() == Http2Connection::State::Running && !connection->conn_.has_active_streams()) {
        connection->request_drain();
    }
}

Http2ServerConnection::~Http2ServerConnection() {
    cancel_idle_timer();
    FIBER_ASSERT(!worker_hook_.linked());
}

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
