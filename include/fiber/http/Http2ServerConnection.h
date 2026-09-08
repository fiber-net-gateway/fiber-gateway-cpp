#ifndef FIBER_HTTP_HTTP2_SERVER_CONNECTION_H
#define FIBER_HTTP_HTTP2_SERVER_CONNECTION_H

#include <atomic>
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
#include "HttpTransport.h"
#include "ServerRequestFactory.h"

namespace fiber::http {

// Server-side owner of one HTTP/2 session.
//
// Intended to live on the frame of a per-connection serve coroutine: the
// Http2Connection is held by value so a session costs no individual heap
// allocation, and the owning Http2ServerWorker reaches it through an
// intrusive hook instead of shared ownership. All methods must be called on
// the transport's event loop.
class Http2ServerConnection : public common::NonCopyable, public common::NonMovable {
public:
    // `request_factory` must outlive this connection.
    Http2ServerConnection(event::EventLoop &loop, Http2Connection::Options options,
                          ServerRequestFactory &request_factory) noexcept;
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
    event::EventLoop *loop_;
    ServerRequestFactory *request_factory_;
    Http2Connection conn_;
    Http2CloseGate close_gate_;
    // Membership slot in the owning worker's list (Http2Stream::owned_hook_
    // pattern: private hook, friend reaches it by offset).
    common::IntrusiveListHook worker_hook_{};

    friend class Http2ServerWorker;
    friend class Http2ConnectionRegistry;
};

// Per-loop registry of live HTTP/2 server connections, Http3Server-shard
// style. Connections link themselves in from their serve-coroutine frames
// through their intrusive hooks, so registration costs no allocation; the list
// is only ever walked or mutated on its own loop, so it needs no lock. Held
// via shared_ptr by the server runtime: a shutdown walk posted to this
// worker's loop must stay valid even if the runtime itself is destroyed
// first.
class Http2ServerWorker : public common::NonCopyable, public common::NonMovable {
public:
    explicit Http2ServerWorker(event::EventLoop &loop) noexcept : loop_(loop) {}

    [[nodiscard]] event::EventLoop &loop() const noexcept { return loop_; }

    // Register/unregister from a connection's serve coroutine, on this loop.
    void link(Http2ServerConnection &connection) noexcept { connections_.push_back(connection); }
    void unlink(Http2ServerConnection &connection) noexcept { connections_.erase(connection); }
    [[nodiscard]] bool empty() const noexcept { return connections_.empty(); }

    // Collapses repeated shutdown requests into one pending walk per worker.
    // A connection that links after the walk ran observes the server closing
    // and shuts itself down.
    bool claim_close_walk() noexcept {
        bool expected = false;
        return close_walk_posted_.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
    }
    void release_close_walk() noexcept { close_walk_posted_.store(false, std::memory_order_release); }
    // Requests shutdown of every registered connection. Must run on this
    // worker's loop.
    void shutdown_connections() noexcept {
        for (Http2ServerConnection *connection = connections_.front(); connection != nullptr;
             connection = connections_.next_of(*connection)) {
            connection->request_shutdown();
        }
    }

private:
    // Http2ServerConnection is not standard-layout (it holds an Http2Connection
    // by value, which owns unique_ptrs), so offsetof warns here. It is still
    // well-defined enough in practice: the type is non-polymorphic and has no
    // virtual base, which is what container_of actually needs.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
    using ConnectionList = common::IntrusiveList<Http2ServerConnection, offsetof(Http2ServerConnection, worker_hook_)>;
#pragma GCC diagnostic pop

    event::EventLoop &loop_;
    ConnectionList connections_{};
    std::atomic<bool> close_walk_posted_{false};
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_SERVER_CONNECTION_H
