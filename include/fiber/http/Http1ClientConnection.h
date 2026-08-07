#ifndef FIBER_HTTP_HTTP1_CLIENT_CONNECTION_H
#define FIBER_HTTP_HTTP1_CLIENT_CONNECTION_H

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../event/EventLoop.h"
#include "../net/SocketAddress.h"
#include "../net/TcpSocketOptions.h"
#include "../net/TlsContext.h"

namespace fiber::http {

class ClientHttp1Exchange;
class HttpTransport;

struct Http1ClientConnectionOptions {
    net::SocketAddress peer_addr{};
    net::TcpSocketOptions tcp{.no_delay = net::TcpOptionMode::Enabled};
    net::TlsOptions tls{};
};

class Http1ClientConnection : public common::NonCopyable, public common::NonMovable {
public:
    Http1ClientConnection(event::EventLoop &loop, Http1ClientConnectionOptions options) noexcept;
    ~Http1ClientConnection();

    // timeout applies to the TCP connect phase. TLS handshake timeout is configured separately.
    fiber::async::Task<common::IoResult<void>> connect(std::chrono::milliseconds timeout) noexcept;
    void close() noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool idle() const noexcept;
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] bool reusable() const noexcept;
    [[nodiscard]] std::uint64_t request_count() const noexcept { return request_count_; }

    [[nodiscard]] event::EventLoop &loop() const noexcept;
    [[nodiscard]] const Http1ClientConnectionOptions &options() const noexcept { return options_; }

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
        ConnectedIdle,
        Busy,
        Closed,
    };

    static Http1ClientConnectionOptions normalize_options(Http1ClientConnectionOptions options) noexcept;
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
    Http1ClientConnectionOptions options_{};
    net::TlsContext tls_ctx_;
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
