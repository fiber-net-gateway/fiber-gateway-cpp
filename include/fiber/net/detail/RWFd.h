#ifndef FIBER_NET_DETAIL_RW_FD_H
#define FIBER_NET_DETAIL_RW_FD_H

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <new>

#include "../../common/Assert.h"
#include "../../common/IoError.h"
#include "../../common/NonCopyable.h"
#include "../../common/NonMovable.h"
#include "../../event/EventLoop.h"
#include "Efd.h"

namespace fiber::net::detail {

class RWFd;

struct RWFdWaiterBase {
    using CompleteCallback = void (*)(RWFdWaiterBase *waiter, fiber::common::IoErr err) noexcept;

    RWFd *rwfd_ = nullptr;
    fiber::event::IoEvent event_{fiber::event::IoEvent::None};
    fiber::common::IoErr err_{fiber::common::IoErr::None};
    std::coroutine_handle<> coro_ = nullptr;
    CompleteCallback complete_callback_ = nullptr;

    static void on_event(void *ctx, fiber::common::IoErr err) noexcept;
    void complete(fiber::common::IoErr err) noexcept;
};

struct RWFdLocalThreadWaiter : RWFdWaiterBase {};
struct RWFdCrossThreadWaiter;

enum class RWFdWaiterState : std::uint8_t {
    Notify_Watch,
    Notify_Resume,
    Watching_Event,
    Request_Cancel,
    Waiting_Cancel,
    Canceled,
};

class RWFd : public common::NonCopyable, public common::NonMovable {
public:
    using ReadyCallback = void (*)(void *ctx, fiber::common::IoErr err) noexcept;

    template<fiber::event::IoEvent Event>
    class WaitAwaiter;

    using WaitReadableAwaiter = WaitAwaiter<fiber::event::IoEvent::Read>;
    using WaitWritableAwaiter = WaitAwaiter<fiber::event::IoEvent::Write>;

    enum class Kind : std::uint8_t { Raw, Stream, Datagram };
    explicit RWFd(fiber::event::EventLoop &loop, Kind kind = Kind::Raw);
    RWFd(fiber::event::EventLoop &loop, int fd, Kind kind = Kind::Raw);

    // Raw fd consumers opt out of cached syscall suppression. Off-loop I/O
    // automatically makes this choice sticky until the next attach.
    void use_external_io() noexcept { external_io_.store(true, std::memory_order_release); }
    common::IoErr prepare_io(event::IoEvent direction) noexcept;
    void finish_io(event::IoEvent direction, common::IoErr error, bool exhausted = false) noexcept;
    ~RWFd();

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] fiber::event::EventLoop &loop() const noexcept;
    [[nodiscard]] bool terminal() const noexcept { return terminal_; }
    // Owner-loop view of whether the peer has closed its write side or the
    // connection has terminated. Lets idle pooled connections be discarded
    // before a request is written onto a dead socket.
    [[nodiscard]] bool peer_closed() const noexcept { return read_hangup_ || terminal_; }
    [[nodiscard]] fiber::common::IoErr terminal_error() const noexcept { return terminal_error_; }

    fiber::common::IoErr attach(int fd) noexcept;
    int release_fd() noexcept;
    void close();

    // Read/write subscriptions persist, but readiness is delivered once per
    // kernel hint or newly installed subscription. Budgeted consumers must post
    // their own continuation; clearing a subscription does not clear readiness.
    // Setters never invoke callbacks synchronously. Callback contexts must outlive
    // their cancellation notification (or be removed before close).
    // Read/write callbacks receive None on readiness and Canceled when the fd is
    // closed. The terminal callback is one-shot and independent from both
    // directions; it observes ERR/HUP or an explicit terminal syscall error, but
    // not RDHUP/EOF. After removing its callback slot, a callback may
    // synchronously destroy this RWFd; event dispatch stops immediately in that
    // case. Clear operations only remove the matching callback and ctx.
    fiber::common::IoErr set_read_callback(ReadyCallback callback, void *ctx) noexcept;
    fiber::common::IoErr set_write_callback(ReadyCallback callback, void *ctx) noexcept;
    fiber::common::IoErr set_terminal_callback(ReadyCallback callback, void *ctx) noexcept;
    fiber::common::IoErr clear_read_callback(ReadyCallback callback, void *ctx) noexcept;
    fiber::common::IoErr clear_write_callback(ReadyCallback callback, void *ctx) noexcept;
    fiber::common::IoErr clear_terminal_callback(ReadyCallback callback, void *ctx) noexcept;
    void mark_terminal(fiber::common::IoErr error) noexcept;

    [[nodiscard]] WaitReadableAwaiter
    wait_readable(std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    [[nodiscard]] WaitWritableAwaiter
    wait_writable(std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;

private:
    friend struct RWFdWaiterBase;
    friend struct RWFdCrossThreadWaiter;

    template<fiber::event::IoEvent Event>
    friend class WaitAwaiter;

    class DispatchGuard {
    public:
        explicit DispatchGuard(RWFd &owner) noexcept;
        ~DispatchGuard() noexcept;

        DispatchGuard(const DispatchGuard &) = delete;
        DispatchGuard &operator=(const DispatchGuard &) = delete;
        DispatchGuard(DispatchGuard &&) = delete;
        DispatchGuard &operator=(DispatchGuard &&) = delete;

        [[nodiscard]] bool owner_destroyed() const noexcept { return owner_destroyed_; }

    private:
        RWFd *owner_ = nullptr;
        bool owner_destroyed_ = false;
    };

    common::IoResult<bool> begin_wait(RWFdWaiterBase *waiter) noexcept;
    fiber::common::IoErr cancel_wait(RWFdWaiterBase *waiter) noexcept;
    bool remove_callback(fiber::event::IoEvent event, ReadyCallback callback, void *ctx) noexcept;

    static void on_efd_events(void *owner, fiber::event::IoEvent events);
    void handle_events(fiber::event::IoEvent events);
    [[nodiscard]] bool has_callbacks() const noexcept;
    [[nodiscard]] fiber::event::IoEvent active_events() const noexcept;
    common::IoErr ensure_registered() noexcept;
    common::IoResult<bool> check_ready(event::IoEvent direction) noexcept;
    common::IoErr install_callback(event::IoEvent direction, ReadyCallback callback, void *ctx) noexcept;
    void queue_ready(event::IoEvent events) noexcept;
    void dispatch_ready(event::IoEvent events);
    static void on_deferred_ready(RWFd *owner) noexcept;
    static void on_loop_stop(RWFd *owner) noexcept;
    enum class Readiness : std::uint8_t { Unknown, Ready, Blocked };
    Readiness read_ready_ = Readiness::Unknown;
    Readiness write_ready_ = Readiness::Unknown;
    const Kind kind_;
    std::atomic<bool> external_io_;
    bool read_hangup_ = false;
    std::uint64_t read_generation_ = 0;
    std::uint64_t write_generation_ = 0;
    std::uint64_t terminal_generation_ = 0;
    event::IoEvent pending_ready_ = event::IoEvent::None;
    event::EventLoop::DeferEntry ready_entry_{};
    event::EventLoop::StopEntry stop_entry_{};

    Efd efd_;
    ReadyCallback read_callback_ = nullptr;
    void *read_callback_ctx_ = nullptr;
    ReadyCallback write_callback_ = nullptr;
    void *write_callback_ctx_ = nullptr;
    ReadyCallback terminal_callback_ = nullptr;
    void *terminal_callback_ctx_ = nullptr;
    fiber::common::IoErr terminal_error_ = fiber::common::IoErr::None;
    bool terminal_ = false;
    bool *dispatch_destroyed_observer_ = nullptr;
};

struct RWFdCrossThreadWaiter : RWFdWaiterBase {
    fiber::event::EventLoop *loop_ = nullptr;
    fiber::event::EventLoop::NotifyEntry notify_entry_{};
    fiber::event::EventLoop::NotifyEntry cancel_entry_{};
    std::atomic<RWFdWaiterState> state_{RWFdWaiterState::Notify_Watch};

    void cancel_wait() noexcept;

    static void on_complete(RWFdWaiterBase *base, fiber::common::IoErr err) noexcept;
    static void do_notify_resume(RWFdCrossThreadWaiter *waiter) noexcept;
    static void on_notify_watch(RWFdCrossThreadWaiter *waiter) noexcept;
    static void on_notify_cancel(RWFdCrossThreadWaiter *waiter) noexcept;
    static void on_notify_resume(RWFdCrossThreadWaiter *waiter) noexcept;
};

template<fiber::event::IoEvent Event>
class RWFd::WaitAwaiter : public RWFdLocalThreadWaiter {
public:
    explicit WaitAwaiter(RWFd &rwfd, std::chrono::milliseconds timeout) noexcept : timeout_(timeout) {
        static_assert(Event == fiber::event::IoEvent::Read || Event == fiber::event::IoEvent::Write);
        rwfd_ = &rwfd;
        event_ = Event;
        complete_callback_ = &WaitAwaiter::on_complete;
    }

    WaitAwaiter(const WaitAwaiter &) = delete;
    WaitAwaiter &operator=(const WaitAwaiter &) = delete;
    WaitAwaiter(WaitAwaiter &&) = delete;
    WaitAwaiter &operator=(WaitAwaiter &&) = delete;

    ~WaitAwaiter() {
        cancel_timer();
        if (!waiting_) {
            FIBER_ASSERT(waiter_ == nullptr);
            return;
        }
        if (waiter_) {
            FIBER_ASSERT(!rwfd_->loop().in_loop());
            auto *waiter = waiter_;
            waiter_ = nullptr;
            waiter->cancel_wait();
            return;
        }
        FIBER_ASSERT(rwfd_->loop().in_loop());
        (void) rwfd_->cancel_wait(this);
    }

    bool await_ready() noexcept {
        if (timeout_ > std::chrono::milliseconds::zero()) {
            return false;
        }
        err_ = fiber::common::IoErr::TimedOut;
        completed_ = true;
        return true;
    }

    bool await_suspend(std::coroutine_handle<> handle) noexcept {
        coro_ = handle;
        err_ = fiber::common::IoErr::None;
        completed_ = false;
        waiting_ = true;
        origin_loop_ = &fiber::event::EventLoop::current();

        if (rwfd_->loop().in_loop()) {
            auto result = rwfd_->begin_wait(this);
            if (!result || !*result) {
                err_ = result ? fiber::common::IoErr::None : result.error();
                completed_ = true;
                waiting_ = false;
                return false;
            }
            arm_timer();
            return true;
        }

        rwfd_->use_external_io();
        auto *waiter = new (std::nothrow) RWFdCrossThreadWaiter();
        if (!waiter) {
            err_ = fiber::common::IoErr::NoMem;
            completed_ = true;
            waiting_ = false;
            return false;
        }
        waiter->rwfd_ = rwfd_;
        waiter->event_ = Event;
        waiter->coro_ = handle;
        waiter->complete_callback_ = &RWFdCrossThreadWaiter::on_complete;
        waiter->loop_ = origin_loop_;
        waiter_ = waiter;
        rwfd_->loop()
                .post<RWFdCrossThreadWaiter, &RWFdCrossThreadWaiter::notify_entry_,
                      &RWFdCrossThreadWaiter::on_notify_watch>(*waiter);
        arm_timer();
        return true;
    }

    fiber::common::IoResult<void> await_resume() noexcept {
        waiting_ = false;
        cancel_timer();
        if (completed_) {
            completed_ = false;
            if (err_ == fiber::common::IoErr::None) {
                return {};
            }
            return std::unexpected(err_);
        }

        fiber::common::IoErr err = err_;
        RWFdCrossThreadWaiter *waiter = waiter_;
        if (waiter) {
            err = waiter->err_;
            waiter_ = nullptr;
            delete waiter;
        }
        if (err == fiber::common::IoErr::None) {
            return {};
        }
        return std::unexpected(err);
    }

private:
    void arm_timer() noexcept {
        if (timeout_ == std::chrono::milliseconds::max()) {
            return;
        }
        FIBER_ASSERT(origin_loop_ != nullptr);
        origin_loop_->post_at<WaitAwaiter, &WaitAwaiter::timer_entry_, &WaitAwaiter::on_timeout>(
                origin_loop_->now() + timeout_, *this);
    }

    void cancel_timer() noexcept {
        if (!timer_entry_.is_in_heap()) {
            return;
        }
        FIBER_ASSERT(origin_loop_ != nullptr);
        FIBER_ASSERT(origin_loop_->in_loop());
        origin_loop_->cancel<WaitAwaiter, &WaitAwaiter::timer_entry_>(*this);
    }

    static void on_complete(RWFdWaiterBase *base, fiber::common::IoErr err) noexcept {
        auto *awaiter = static_cast<WaitAwaiter *>(static_cast<RWFdLocalThreadWaiter *>(base));
        awaiter->err_ = err;
        awaiter->cancel_timer();
        awaiter->coro_.resume();
    }

    static void on_timeout(WaitAwaiter *awaiter) noexcept {
        FIBER_ASSERT(awaiter != nullptr);
        FIBER_ASSERT(awaiter->waiting_);

        if (awaiter->waiter_) {
            RWFdCrossThreadWaiter *waiter = awaiter->waiter_;
            awaiter->waiter_ = nullptr;
            waiter->cancel_wait();
        } else {
            (void) awaiter->rwfd_->cancel_wait(awaiter);
        }
        awaiter->waiting_ = false;
        awaiter->err_ = fiber::common::IoErr::TimedOut;
        awaiter->coro_.resume();
    }

    std::chrono::milliseconds timeout_{};
    fiber::event::EventLoop *origin_loop_ = nullptr;
    fiber::event::EventLoop::TimerEntry timer_entry_{};
    bool waiting_ = false;
    bool completed_ = false;
    RWFdCrossThreadWaiter *waiter_ = nullptr;
};

} // namespace fiber::net::detail

#endif // FIBER_NET_DETAIL_RW_FD_H
