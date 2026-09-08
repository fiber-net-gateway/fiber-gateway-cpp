#ifndef FIBER_HTTP_HTTP1_CLIENT_CONNECTION_H
#define FIBER_HTTP_HTTP1_CLIENT_CONNECTION_H

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../event/EventLoop.h"
#include "../net/HappyEyeballs.h"
#include "../net/SocketAddress.h"
#include "../net/TcpSocketOptions.h"
#include "HttpClientTlsOptions.h"

namespace fiber::http {

class ClientHttp1Exchange;
class HttpTransport;

class Http1ClientConnection : public common::NonCopyable, public common::NonMovable {
public:
    explicit Http1ClientConnection(event::EventLoop &loop) noexcept;
    ~Http1ClientConnection();

    // Dials once and, on success, moves to the connected-idle state for good: a connection is
    // never re-dialed, so everything below is a parameter of the single connect rather than
    // retained state. `timeout` covers the TCP phase only; the TLS handshake has its own timeout
    // in HttpClientTlsOptions.
    //
    // Every argument is borrowed until the returned task completes, including the storage behind
    // `addresses` and behind the views in `tls`.
    fiber::async::Task<common::IoResult<void>>
    connect(const net::SocketAddress &peer, std::chrono::milliseconds timeout,
            const net::TcpSocketOptions &tcp = net::kNoDelayTcpSocketOptions) noexcept;
    fiber::async::Task<common::IoResult<void>>
    connect(const net::SocketAddress &peer, std::chrono::milliseconds timeout, const HttpClientTlsOptions &tls,
            const net::TcpSocketOptions &tcp = net::kNoDelayTcpSocketOptions) noexcept;
    // Races an already-resolved address set during the TCP phase. The first TCP success proceeds
    // through the same socket-option and optional TLS setup as the single-address overloads.
    fiber::async::Task<common::IoResult<void>>
    connect(std::span<const net::SocketAddress> addresses, const net::HappyEyeballsOptions &options,
            const net::TcpSocketOptions &tcp = net::kNoDelayTcpSocketOptions) noexcept;
    fiber::async::Task<common::IoResult<void>>
    connect(std::span<const net::SocketAddress> addresses, const net::HappyEyeballsOptions &options,
            const HttpClientTlsOptions &tls, const net::TcpSocketOptions &tcp = net::kNoDelayTcpSocketOptions) noexcept;
    void close() noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool idle() const noexcept;
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] bool reusable() const noexcept;
    [[nodiscard]] std::uint64_t request_count() const noexcept { return request_count_; }

    [[nodiscard]] event::EventLoop &loop() const noexcept;
    // The address this connection actually reached, which for a raced address set is the winner
    // of the TCP phase. Empty until connect() succeeds.
    [[nodiscard]] const net::SocketAddress &peer_addr() const noexcept { return peer_addr_; }

private:
    using IoTask = fiber::async::Task<common::IoResult<std::size_t>>;

    class IoAwaiter {
    public:
        IoAwaiter(Http1ClientConnection &connection, IoAwaiter *&slot, IoTask task) noexcept;
        IoAwaiter(const IoAwaiter &) = delete;
        IoAwaiter &operator=(const IoAwaiter &) = delete;
        IoAwaiter(IoAwaiter &&) = delete;
        IoAwaiter &operator=(IoAwaiter &&) = delete;
        ~IoAwaiter();

        bool await_ready() noexcept;
        std::coroutine_handle<> await_suspend(std::coroutine_handle<> handle) noexcept;
        common::IoResult<std::size_t> await_resume() noexcept;

        void prepare_cancel(common::IoErr reason) noexcept;
        void schedule_cancel_resume() noexcept;

    private:
        enum class State : std::uint8_t {
            Created,
            Waiting,
            ReadyError,
            CancelPrepared,
            ResumeQueued,
            CanceledReady,
            Completed,
            Abandoned,
        };

        void detach() noexcept;
        static void on_cancel_resume(IoAwaiter *awaiter) noexcept;

        Http1ClientConnection &connection_;
        IoAwaiter **slot_ = nullptr;
        event::EventLoop *loop_ = nullptr;
        IoTask task_;
        IoTask::Awaiter task_awaiter_;
        std::coroutine_handle<> handle_{};
        event::EventLoop::DeferEntry resume_entry_{};
        common::IoErr result_err_ = common::IoErr::None;
        State state_ = State::Created;
    };

    enum class State : std::uint8_t {
        Init,
        Connecting,
        ConnectedIdle,
        Busy,
        Closed,
    };

    class ConnectStateGuard {
    public:
        explicit ConnectStateGuard(Http1ClientConnection &connection) noexcept : connection_(connection) {}
        ~ConnectStateGuard();

    private:
        Http1ClientConnection &connection_;
    };

    common::IoErr begin_connect() noexcept;
    // An engaged `peer` selects the single-address path; otherwise `addresses` is raced, and an
    // empty set there is a failure rather than a fallback. The caller-facing overloads copy the
    // small, trivially copyable parameter structs in so only the pointees stay borrowed.
    fiber::async::Task<common::IoResult<void>> connect_impl(std::optional<net::SocketAddress> peer,
                                                            std::span<const net::SocketAddress> addresses,
                                                            net::HappyEyeballsOptions options,
                                                            net::TcpSocketOptions tcp,
                                                            std::optional<HttpClientTlsOptions> tls) noexcept;
    void assert_active_loop() const noexcept;
    void mark_unusable() noexcept;
    void record_request_started() noexcept;
    [[nodiscard]] bool acquire_exchange() noexcept;
    [[nodiscard]] bool exchange_active() const noexcept;
    void release_exchange(bool keepalive) noexcept;
    void fail_exchange(common::IoErr reason) noexcept;
    void fail_active_exchange(common::IoErr reason) noexcept;
    void on_io_awaiter_destroyed() noexcept;
    IoAwaiter wait_transport_read(IoTask task) noexcept;
    IoAwaiter wait_transport_write(IoTask task) noexcept;

    friend class ClientHttp1Exchange;

    event::EventLoop *loop_ = nullptr;
    net::SocketAddress peer_addr_{};
    std::unique_ptr<HttpTransport> transport_;
    event::EventLoop *active_loop_ = nullptr;
    IoAwaiter *reader_ = nullptr;
    IoAwaiter *writer_ = nullptr;
    std::uint64_t request_count_ = 0;
    State state_ = State::Init;
    bool keepalive_usable_ = false;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP1_CLIENT_CONNECTION_H
