#ifndef FIBER_HTTP_HTTP2_SERVER_CONNECTION_H
#define FIBER_HTTP_HTTP2_SERVER_CONNECTION_H

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <utility>

#include "../async/Task.h"
#include "../common/IntrusiveList.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../event/EventLoop.h"
#include "Http2CloseGate.h"
#include "Http2Connection.h"
#include "Http2ServerOptions.h"
#include "HttpTransport.h"
#include "ServerRequestFactory.h"

namespace fiber::http {

// Server-side owner of one HTTP/2 session.
//
// Intended to live on the frame of a per-connection serve coroutine: the
// Http2Connection is held by value so a session costs no individual heap
// allocation, and the owning Http2ConnectionRegistry reaches it through an
// intrusive hook instead of shared ownership. All methods must be called on
// the transport's event loop.
class Http2ServerConnection : public common::NonCopyable, public common::NonMovable {
public:
    // `request_factory` must outlive this connection.
    Http2ServerConnection(event::EventLoop &loop, Http2Connection::Options options,
                          ServerRequestFactory &request_factory,
                          std::chrono::milliseconds idle_timeout = Http2ServerOptions{}.idle_timeout) noexcept;
    ~Http2ServerConnection();

    // Starts the session; on success the transport is owned and driven to
    // closure by the connection itself (there is no run coroutine to await).
    // A failed start leaves the connection inert.
    common::IoErr start(std::unique_ptr<HttpTransport> transport) noexcept;

    // Resolves once the connection has fully closed; observers see the closure
    // first. Armed in the constructor, so this is safe even for a connection
    // that fails to start or closes immediately.
    fiber::async::Task<Http2Connection::CloseResult> wait_closed() noexcept;

    // Graceful shutdown: sends GOAWAY so the peer opens no new streams, and
    // lets the streams already running finish. The connection closes itself
    // once the last one ends. Idempotent.
    void request_drain() noexcept;

    // Hard teardown: aborts every stream and the transport. Idempotent and safe
    // on a connection that is already closing or closed.
    void request_shutdown() noexcept;

    [[nodiscard]] Http2Connection &http2() noexcept { return conn_; }
    [[nodiscard]] const Http2Connection &http2() const noexcept { return conn_; }

private:
    static const Http2Connection::Ops &connection_ops() noexcept;
    static Http2Stream::Lease create_peer_stream(void *ctx, std::uint32_t id, Http2Connection &conn) noexcept;
    static void on_state_change(void *ctx, Http2Connection &connection) noexcept;
    static void on_capacity_change(void *ctx, Http2Connection &connection) noexcept;
    static void on_idle_timer(Http2ServerConnection *connection) noexcept;
    void sync_idle_timer() noexcept;
    void cancel_idle_timer() noexcept;
    event::EventLoop *loop_;
    ServerRequestFactory *request_factory_;
    Http2Connection conn_;
    Http2CloseGate close_gate_;
    const std::chrono::milliseconds idle_timeout_;
    event::EventLoop::TimerEntry idle_timer_entry_{};
    // Membership slot in the owning worker's list (Http2Stream::owned_hook_
    // pattern: private hook, friend reaches it by offset).
    common::IntrusiveListHook worker_hook_{};

    friend class Http2ConnectionRegistry;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_SERVER_CONNECTION_H
