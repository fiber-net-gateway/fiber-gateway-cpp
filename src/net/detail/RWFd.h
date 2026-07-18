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

    explicit RWFd(fiber::event::EventLoop &loop);
    RWFd(fiber::event::EventLoop &loop, int fd);
    ~RWFd();

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] fiber::event::EventLoop &loop() const noexcept;

    fiber::common::IoErr attach(int fd) noexcept;
    int release_fd() noexcept;
    void close();

    // Callbacks receive None on readiness and Canceled when the fd is closed. They
    // may update callback registration, but must keep this RWFd alive until event
    // dispatch returns. Clear operations only remove the matching callback and ctx.
    fiber::common::IoErr set_read_callback(ReadyCallback callback, void *ctx) noexcept;
    fiber::common::IoErr set_write_callback(ReadyCallback callback, void *ctx) noexcept;
    fiber::common::IoErr clear_read_callback(ReadyCallback callback, void *ctx) noexcept;
    fiber::common::IoErr clear_write_callback(ReadyCallback callback, void *ctx) noexcept;

    [[nodiscard]] WaitReadableAwaiter
    wait_readable(std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    [[nodiscard]] WaitWritableAwaiter
    wait_writable(std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;

private:
    friend struct RWFdWaiterBase;
    friend struct RWFdCrossThreadWaiter;

    template<fiber::event::IoEvent Event>
    friend class WaitAwaiter;

    fiber::common::IoErr begin_wait(RWFdWaiterBase *waiter) noexcept;
    fiber::common::IoErr cancel_wait(RWFdWaiterBase *waiter) noexcept;
    bool remove_callback(fiber::event::IoEvent event, ReadyCallback callback, void *ctx) noexcept;

    static void on_efd_events(void *owner, fiber::event::IoEvent events);
    void handle_events(fiber::event::IoEvent events);
    [[nodiscard]] bool has_callbacks() const noexcept;
    [[nodiscard]] fiber::event::IoEvent active_events() const noexcept;
    fiber::common::IoErr sync_interest() noexcept;

    Efd efd_;
    ReadyCallback read_callback_ = nullptr;
    void *read_callback_ctx_ = nullptr;
    ReadyCallback write_callback_ = nullptr;
    void *write_callback_ctx_ = nullptr;
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
            fiber::common::IoErr err = rwfd_->begin_wait(this);
            if (err != fiber::common::IoErr::None) {
                err_ = err;
                completed_ = true;
                waiting_ = false;
                return false;
            }
            arm_timer();
            return true;
        }

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
