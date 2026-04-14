#ifndef FIBER_NET_DETAIL_RW_FD_H
#define FIBER_NET_DETAIL_RW_FD_H

#include <atomic>
#include <concepts>
#include <coroutine>
#include <cstdint>
#include <new>
#include <type_traits>

#include "../../common/Assert.h"
#include "../../common/IoError.h"
#include "../../common/NonCopyable.h"
#include "../../common/NonMovable.h"
#include "../../event/EventLoop.h"
#include "Efd.h"

namespace fiber::net::detail {

class RWFd;

struct RWFdWaiterBase {
    RWFd *rwfd_ = nullptr;
    fiber::event::IoEvent interested_{fiber::event::IoEvent::None};
    fiber::event::IoEvent ready_{fiber::event::IoEvent::None};
    fiber::common::IoErr err_{fiber::common::IoErr::None};
    std::coroutine_handle<> coro_ = nullptr;
};

struct RWFdLocalThreadWaiter : RWFdWaiterBase {};
struct RWFdCrossThreadWaiter;

template<typename T>
concept RWFdWaiter = std::same_as<std::remove_cvref_t<T>, RWFdLocalThreadWaiter> ||
                     std::same_as<std::remove_cvref_t<T>, RWFdCrossThreadWaiter>;

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
    class WaitEventAwaiter;

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

    [[nodiscard]] WaitEventAwaiter wait_event(fiber::event::IoEvent interested) noexcept;
    [[nodiscard]] WaitReadableAwaiter wait_readable() noexcept;
    [[nodiscard]] WaitWritableAwaiter wait_writable() noexcept;

private:
    friend struct RWFdCrossThreadWaiter;
    friend class WaitEventAwaiter;

    template<typename Waiter>
        requires(RWFdWaiter<Waiter>)
    fiber::common::IoErr begin_wait(Waiter *waiter) noexcept {
        FIBER_ASSERT(loop().in_loop());
        FIBER_ASSERT(waiter != nullptr);
        FIBER_ASSERT(fiber::event::any(waiter->interested_));
        if (!valid()) {
            return fiber::common::IoErr::BadFd;
        }
        if (waiter_ != nullptr) {
            return fiber::common::IoErr::Busy;
        }
        fiber::common::IoErr err = efd_.watch_add(waiter->interested_);
        if (err != fiber::common::IoErr::None) {
            return err;
        }
        if constexpr (std::same_as<std::remove_cvref_t<Waiter>, RWFdLocalThreadWaiter>) {
            waiter_ = waiter;
            local_waiting_ = true;
        } else {
            waiter_ = waiter;
            local_waiting_ = false;
        }
        return fiber::common::IoErr::None;
    }

    template<typename Waiter>
        requires(RWFdWaiter<Waiter>)
    fiber::common::IoErr cancel_wait(Waiter *waiter) noexcept {
        FIBER_ASSERT(loop().in_loop());
        FIBER_ASSERT(waiter != nullptr);
        if (waiter_ != waiter) {
            return fiber::common::IoErr::None;
        }
        waiter_ = nullptr;
        local_waiting_ = false;
        return efd_.watch_del(waiter->interested_);
    }

    static void on_efd_events(void *owner, fiber::event::IoEvent events);
    void handle_events(fiber::event::IoEvent events);
    [[nodiscard]] bool has_waiters() const noexcept;

    Efd efd_;
    bool local_waiting_ = false;
    union {
        RWFdLocalThreadWaiter *local_waiter_ = nullptr;
        RWFdCrossThreadWaiter *cross_waiter_;
        RWFdWaiterBase *waiter_base_;
        void *waiter_;
    };
};

struct RWFdCrossThreadWaiter : RWFdWaiterBase {
    fiber::event::EventLoop *loop_ = nullptr;
    fiber::event::EventLoop::NotifyEntry notify_entry_{};
    fiber::event::EventLoop::NotifyEntry cancel_entry_{};
    std::atomic<RWFdWaiterState> state_{RWFdWaiterState::Notify_Watch};

    void cancel_wait() noexcept;

    static void do_notify_resume(RWFdCrossThreadWaiter *waiter) noexcept;
    static void on_notify_watch(RWFdCrossThreadWaiter *waiter) noexcept;
    static void on_notify_cancel(RWFdCrossThreadWaiter *waiter) noexcept;
    static void on_notify_resume(RWFdCrossThreadWaiter *waiter) noexcept;
};

class RWFd::WaitEventAwaiter : public RWFdLocalThreadWaiter {
public:
    WaitEventAwaiter(RWFd &rwfd, fiber::event::IoEvent interested) noexcept {
        rwfd_ = &rwfd;
        interested_ = interested;
    }

    WaitEventAwaiter(const WaitEventAwaiter &) = delete;
    WaitEventAwaiter &operator=(const WaitEventAwaiter &) = delete;
    WaitEventAwaiter(WaitEventAwaiter &&) = delete;
    WaitEventAwaiter &operator=(WaitEventAwaiter &&) = delete;

    ~WaitEventAwaiter() {
        if (!waiting_) {
            FIBER_ASSERT(waiter_ == nullptr);
            return;
        }
        if (waiter_) {
            FIBER_ASSERT(!rwfd_->loop().in_loop());
            auto *waiter = waiter_;
            waiter->cancel_wait();
            waiter_ = nullptr;
            return;
        }
        FIBER_ASSERT(rwfd_->loop().in_loop());
        (void) rwfd_->cancel_wait<RWFdLocalThreadWaiter>(this);
    }

    bool await_ready() noexcept {
        if (!fiber::event::any(interested_)) {
            err_ = fiber::common::IoErr::Invalid;
            completed_ = true;
            return true;
        }
        return false;
    }

    bool await_suspend(std::coroutine_handle<> handle) {
        coro_ = handle;
        ready_ = fiber::event::IoEvent::None;
        err_ = fiber::common::IoErr::None;
        completed_ = false;
        waiting_ = true;

        if (rwfd_->loop().in_loop()) {
            fiber::common::IoErr err = rwfd_->begin_wait<RWFdLocalThreadWaiter>(this);
            if (err != fiber::common::IoErr::None) {
                err_ = err;
                completed_ = true;
                waiting_ = false;
                return false;
            }
            return true;
        }

        auto *current = fiber::event::EventLoop::current_or_null();
        FIBER_ASSERT(current != nullptr);
        auto *waiter = new (std::nothrow) RWFdCrossThreadWaiter();
        if (!waiter) {
            err_ = fiber::common::IoErr::NoMem;
            completed_ = true;
            waiting_ = false;
            return false;
        }
        waiter->rwfd_ = rwfd_;
        waiter->interested_ = interested_;
        waiter->ready_ = fiber::event::IoEvent::None;
        waiter->coro_ = handle;
        waiter->loop_ = current;
        waiter_ = waiter;
        rwfd_->loop()
                .post<RWFdCrossThreadWaiter, &RWFdCrossThreadWaiter::notify_entry_,
                      &RWFdCrossThreadWaiter::on_notify_watch>(*waiter);
        return true;
    }

    fiber::common::IoResult<fiber::event::IoEvent> await_resume() noexcept {
        waiting_ = false;
        if (completed_) {
            completed_ = false;
            if (err_ == fiber::common::IoErr::None) {
                return ready_;
            }
            return std::unexpected(err_);
        }

        fiber::common::IoErr err = err_;
        fiber::event::IoEvent ready = ready_;
        RWFdCrossThreadWaiter *waiter = waiter_;
        if (waiter) {
            err = waiter->err_;
            ready = waiter->ready_;
            waiter_ = nullptr;
            delete waiter;
        }
        if (err == fiber::common::IoErr::None) {
            return ready;
        }
        return std::unexpected(err);
    }

private:
    bool waiting_ = false;
    bool completed_ = false;
    RWFdCrossThreadWaiter *waiter_ = nullptr;
};

template<fiber::event::IoEvent Event>
class RWFd::WaitAwaiter {
public:
    explicit WaitAwaiter(RWFd &rwfd) noexcept : inner_(rwfd, Event) {}

    WaitAwaiter(const WaitAwaiter &) = delete;
    WaitAwaiter &operator=(const WaitAwaiter &) = delete;
    WaitAwaiter(WaitAwaiter &&) = delete;
    WaitAwaiter &operator=(WaitAwaiter &&) = delete;

    bool await_ready() noexcept { return inner_.await_ready(); }
    bool await_suspend(std::coroutine_handle<> handle) { return inner_.await_suspend(handle); }
    fiber::common::IoResult<void> await_resume() noexcept {
        auto result = inner_.await_resume();
        if (!result) {
            return std::unexpected(result.error());
        }
        return {};
    }

private:
    WaitEventAwaiter inner_;
};

} // namespace fiber::net::detail

#endif // FIBER_NET_DETAIL_RW_FD_H
