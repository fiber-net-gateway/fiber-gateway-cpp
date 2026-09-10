#ifndef FIBER_HTTP_HTTP3_SERVER_CONNECTION_H
#define FIBER_HTTP_HTTP3_SERVER_CONNECTION_H

#include <memory>

#include <fiber/async/WaitGroup.h>
#include <fiber/common/IntrusiveList.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/http/Http3Connection.h>
#include <fiber/http/Http3ServerOptions.h>
#include <fiber/http/HttpExchange.h>
#include <fiber/quic/QuicConnection.h>

namespace fiber::http {

class Http3ConnectionRegistry;

// Server-side owner of one HTTP/3 session: a QuicConnection plus the
// Http3Connection riding on it, both held by value so a session costs one
// allocation.
//
// Lifetime is driven by QUIC: the endpoint hands ownership to a lease, and when
// the last reference goes away QuicConnection calls back into destroy(), which
// finishes the teardown on the connection's loop and then deletes this object.
// `on_closed` fires just before that, on the same loop, so the owner can drop
// its bookkeeping.
class Http3ServerConnection : public common::NonCopyable, public common::NonMovable {
public:
    struct Ops {
        void (*on_closed)(void *owner, Http3ServerConnection &connection) noexcept = nullptr;
    };

    // `options` and `ops` must outlive the connection; `handler` is shared.
    [[nodiscard]] static Http3ServerConnection *create(const quic::QuicConnection::Options &quic_options,
                                                       std::shared_ptr<const HttpHandler> handler,
                                                       const Http3ServerOptions &options, void *owner,
                                                       const Ops &ops) noexcept;

    [[nodiscard]] quic::QuicConnection &quic() noexcept { return quic_; }

    // Starts the HTTP/3 side (control streams, SETTINGS). Loop-affine.
    void start() noexcept;

    [[nodiscard]] bool idle_timer_armed() const noexcept { return idle_timer_.is_in_heap(); }

    // GOAWAY: the peer opens no new requests and the ones already running
    // finish, after which the session closes itself.
    void graceful_shutdown() noexcept { h3_.graceful_shutdown(); }

private:
    Http3ServerConnection(const quic::QuicConnection::Options &quic_options, std::shared_ptr<const HttpHandler> handler,
                          const Http3ServerOptions &options, void *owner, const Ops &ops) noexcept;

    [[nodiscard]] static quic::QuicConnection::Options make_quic_options(const quic::QuicConnection::Options &base,
                                                                         Http3ServerConnection *owner) noexcept;
    [[nodiscard]] static Http3Connection::Options make_http3_options(Http3ServerConnection *owner) noexcept;
    [[nodiscard]] static quic::QuicStream::Lease create_server_request(void *owner, std::uint64_t stream_id,
                                                                       Http3Connection &conn) noexcept;
    static void destroy_connection(void *owner, quic::QuicConnection &connection) noexcept;
    // Http3Connection reports every change in the number of running requests;
    // the session is retired once it has had none for idle_connection_timeout.
    static void on_active_request_count_change(void *owner, Http3Connection &conn) noexcept;
    static void on_idle_timeout(Http3ServerConnection *connection) noexcept;
    void update_idle_timer() noexcept;
    void cancel_idle_timer() noexcept;
    static async::DetachedTask run_start(Http3ServerConnection *connection) noexcept;
    static async::DetachedTask run_cleanup(Http3ServerConnection *connection) noexcept;

    std::shared_ptr<const HttpHandler> handler_;
    const Http3ServerOptions *options_;
    void *owner_;
    const Ops *ops_;
    quic::QuicConnection quic_;
    Http3Connection h3_;
    async::WaitGroup tasks_{};
    event::EventLoop::TimerEntry idle_timer_{};
    // Membership slot in the owning Http3ConnectionRegistry's list.
    common::IntrusiveListHook worker_hook_{};
    bool cleanup_started_ = false;
    bool prepared_ = false;

    friend class Http3ConnectionRegistry;
};

// Per-loop registry of live HTTP/3 sessions, walked only on its own loop.
class Http3ConnectionRegistry {
public:
    void link(Http3ServerConnection &connection) noexcept { connections_.push_back(connection); }
    void unlink(Http3ServerConnection &connection) noexcept { connections_.erase(connection); }
    [[nodiscard]] bool empty() const noexcept { return connections_.empty(); }

    void drain_all() noexcept {
        // The callback must not assume the node survives: read the next
        // pointer first so a connection that unlinks and destroys itself
        // synchronously cannot leave this traversal reading freed memory.
        Http3ServerConnection *connection = connections_.front();
        while (connection != nullptr) {
            Http3ServerConnection *next = connections_.next_of(*connection);
            connection->graceful_shutdown();
            connection = next;
        }
    }

private:
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
    using ConnectionList = common::IntrusiveList<Http3ServerConnection, offsetof(Http3ServerConnection, worker_hook_)>;
#pragma GCC diagnostic pop

    ConnectionList connections_{};
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP3_SERVER_CONNECTION_H
