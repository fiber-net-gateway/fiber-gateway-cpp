#ifndef FIBER_HTTP_HTTP3_SERVER_CONNECTION_H
#define FIBER_HTTP_HTTP3_SERVER_CONNECTION_H
#include <fiber/http/Http3ServerOptions.h>
#include <fiber/http/HttpExchange.h>
#include <memory>
#include "http/Http3ControlStreams.h"
namespace fiber::http {
class Http3ConnectionRegistry;
// One allocation owns QUIC, protocol streams and server request policy.
// The last QUIC lease schedules cleanup; on_closed unlinks the owner before deletion.
class Http3ServerConnection : public common::NonCopyable, public common::NonMovable {
public:
    struct Ops {
        void (*on_closed)(void *, Http3ServerConnection &) noexcept = nullptr;
    };
    // The endpoint, Options and Ops outlive the connection. The handler is shared.
    static Http3ServerConnection *create(quic::QuicUdpEndpoint &, const quic::QuicConnection::Options &,
                                         std::shared_ptr<const HttpHandler>, const Http3ServerOptions &, void *,
                                         const Ops &) noexcept;
    quic::QuicConnection &quic() noexcept { return quic_; }
    const Http3Settings &local_settings() const noexcept { return control_.local_settings(); }
    Http3ConnectionState state() const noexcept { return state_; }
    Http3ErrorCode close_error() const noexcept { return close_error_; }
    std::size_t active_server_request_count() const noexcept { return live_server_requests_; }
    bool closing() const noexcept {
        return state_ == Http3ConnectionState::Draining || state_ == Http3ConnectionState::Closing ||
               state_ == Http3ConnectionState::Closed;
    }
    bool idle_timer_armed() const noexcept { return idle_timer_.is_in_heap(); }
    void start() noexcept;
    void graceful_shutdown() noexcept;
    void close(Http3ErrorCode error = Http3ErrorCode::NoError) noexcept;
    // Request destruction, after response delivery, balances admission here.
    void end_server_request() noexcept;
    // After shutdown/drain or peer closure, join the H3 tasks (startup, control
    // readers, drain) and publish Closed. Does not wait for QUIC endpoint detach
    // or object deletion; it does not initiate a close on its own.
    async::Task<void> wait_closed() noexcept;
    async::Task<void> wait_started() noexcept;

private:
    Http3ServerConnection(quic::QuicUdpEndpoint &, const quic::QuicConnection::Options &,
                          std::shared_ptr<const HttpHandler>, const Http3ServerOptions &, void *, const Ops &) noexcept;
    ~Http3ServerConnection();
    static quic::QuicConnection::Options make_quic_options(const quic::QuicConnection::Options &,
                                                           Http3ServerConnection *) noexcept;
    static Http3Settings make_settings(const Http3ServerOptions &) noexcept;
    static const Http3ControlStreams::Ops &control_ops() noexcept;
    static quic::QuicStream::Lease create_peer_stream(void *, std::uint64_t) noexcept;
    static void on_peer_stream_attached(void *, quic::QuicStream &) noexcept;
    static void on_quic_state_change(void *, quic::QuicConnection &) noexcept;
    static void on_quic_capacity_change(void *, quic::QuicConnection &) noexcept;
    static void destroy_connection(void *, quic::QuicConnection &) noexcept;
    static void on_idle_timeout(Http3ServerConnection *) noexcept;
    void update_idle_timer() noexcept;
    void cancel_idle_timer() noexcept;
    async::DetachedTask run_start() noexcept;
    async::DetachedTask run_graceful_shutdown() noexcept;
    async::Task<void> join_protocol_tasks() noexcept;
    std::shared_ptr<const HttpHandler> handler_;
    const Http3ServerOptions &options_;
    void *owner_;
    const Ops &ops_;
    quic::QuicConnection quic_;
    quic::QuicLocalStreamGate local_stream_gate_;
    Http3ControlStreams control_;
    async::LocalWaitGroup start_tasks_{};
    async::LocalWaitGroup drain_tasks_{};
    async::LocalWaitGroup server_request_group_{};
    std::size_t live_server_requests_ = 0;
    std::uint64_t next_rejected_request_id_ = 0;
    event::EventLoop::TimerEntry idle_timer_{};
    common::IntrusiveListHook worker_hook_{};
    Http3ConnectionState state_ = Http3ConnectionState::Prepared;
    Http3ErrorCode close_error_ = Http3ErrorCode::NoError;
    bool cleanup_started_ = false;
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
