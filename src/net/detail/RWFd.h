#ifndef FIBER_NET_DETAIL_RW_FD_H
#define FIBER_NET_DETAIL_RW_FD_H

#include <atomic>
#include <chrono>
#include <concepts>
#include <coroutine>
#include <cstdint>
#include <new>
#include <type_traits>

#include "../../async/Task.h"
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
    fiber::event::IoEvent event_{fiber::event::IoEvent::None};
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
    using ReadyCallback = void (*)(void *ctx) noexcept;

    template<fiber::event::IoEvent Event>
    class WaitAwaiter;

    using WaitReadableAwaiter = WaitAwaiter<fiber::event::IoEvent::Read>;
    using WaitWritableAwaiter = WaitAwaiter<fiber::event::IoEvent::Write>;
    using WaitTask = fiber::async::Task<fiber::common::IoResult<void>>;

    explicit RWFd(fiber::event::EventLoop &loop);
    RWFd(fiber::event::EventLoop &loop, int fd);
    ~RWFd();

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] fiber::event::EventLoop &loop() const noexcept;

    fiber::common::IoErr attach(int fd) noexcept;
    int release_fd() noexcept;
    void close();

    // Callbacks may update callback registration, but must keep this RWFd alive
    // until event dispatch returns.
    fiber::common::IoErr set_read_callback(ReadyCallback callback, void *ctx) noexcept;
    fiber::common::IoErr set_write_callback(ReadyCallback callback, void *ctx) noexcept;
    fiber::common::IoErr clear_read_callback() noexcept;
    fiber::common::IoErr clear_write_callback() noexcept;

    [[nodiscard]] WaitReadableAwaiter wait_readable() noexcept;
    [[nodiscard]] WaitWritableAwaiter wait_writable() noexcept;
    [[nodiscard]] WaitTask wait_readable(std::chrono::milliseconds timeout) noexcept;
    [[nodiscard]] WaitTask wait_writable(std::chrono::milliseconds timeout) noexcept;

private:
    friend struct RWFdCrossThreadWaiter;

    template<fiber::event::IoEvent Event>
    friend class WaitAwaiter;

    template<typename Waiter>
        requires(RWFdWaiter<Waiter>)
    fiber::common::IoErr begin_wait(Waiter *waiter) noexcept {
        FIBER_ASSERT(loop().in_loop());
        FIBER_ASSERT(waiter != nullptr);
        FIBER_ASSERT(waiter->event_ == fiber::event::IoEvent::Read || waiter->event_ == fiber::event::IoEvent::Write);
        if (!valid()) {
            return fiber::common::IoErr::BadFd;
        }

        constexpr bool kIsLocal = std::same_as<std::remove_cvref_t<Waiter>, RWFdLocalThreadWaiter>;
        RWFdWaiterBase **slot = nullptr;
        bool *local_slot = nullptr;
        if (waiter->event_ == fiber::event::IoEvent::Read) {
            if (read_waiter_ != nullptr || read_callback_ != nullptr) {
                return fiber::common::IoErr::Busy;
            }
            slot = &read_waiter_;
            local_slot = &read_waiter_local_;
        } else {
            if (write_waiter_ != nullptr || write_callback_ != nullptr) {
                return fiber::common::IoErr::Busy;
            }
            slot = &write_waiter_;
            local_slot = &write_waiter_local_;
        }

        *slot = waiter;
        *local_slot = kIsLocal;
        fiber::common::IoErr err = sync_interest();
        if (err != fiber::common::IoErr::None) {
            *slot = nullptr;
            *local_slot = false;
        }
        return err;
    }

    template<typename Waiter>
        requires(RWFdWaiter<Waiter>)
    fiber::common::IoErr cancel_wait(Waiter *waiter) noexcept {
        FIBER_ASSERT(loop().in_loop());
        FIBER_ASSERT(waiter != nullptr);
        bool removed = false;
        if (read_waiter_ == waiter) {
            read_waiter_ = nullptr;
            read_waiter_local_ = false;
            removed = true;
        }
        if (write_waiter_ == waiter) {
            write_waiter_ = nullptr;
            write_waiter_local_ = false;
            removed = true;
        }
        if (!removed) {
            return fiber::common::IoErr::None;
        }
        return sync_interest();
    }

    static void on_efd_events(void *owner, fiber::event::IoEvent events);
    void handle_events(fiber::event::IoEvent events);
    [[nodiscard]] bool has_waiters() const noexcept;
    [[nodiscard]] bool has_callbacks() const noexcept;
    [[nodiscard]] fiber::event::IoEvent active_events() const noexcept;
    fiber::common::IoErr sync_interest() noexcept;

    Efd efd_;
    RWFdWaiterBase *read_waiter_ = nullptr;
    RWFdWaiterBase *write_waiter_ = nullptr;
    ReadyCallback read_callback_ = nullptr;
    void *read_callback_ctx_ = nullptr;
    ReadyCallback write_callback_ = nullptr;
    void *write_callback_ctx_ = nullptr;
    bool read_waiter_local_ = false;
    bool write_waiter_local_ = false;
    bool handling_events_ = false;
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

template<fiber::event::IoEvent Event>
class RWFd::WaitAwaiter : public RWFdLocalThreadWaiter {
public:
    explicit WaitAwaiter(RWFd &rwfd) noexcept {
        static_assert(Event == fiber::event::IoEvent::Read || Event == fiber::event::IoEvent::Write);
        rwfd_ = &rwfd;
        event_ = Event;
    }

    WaitAwaiter(const WaitAwaiter &) = delete;
    WaitAwaiter &operator=(const WaitAwaiter &) = delete;
    WaitAwaiter(WaitAwaiter &&) = delete;
    WaitAwaiter &operator=(WaitAwaiter &&) = delete;

    ~WaitAwaiter() {
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

    bool await_ready() noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle) {
        coro_ = handle;
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
        waiter->event_ = Event;
        waiter->coro_ = handle;
        waiter->loop_ = current;
        waiter_ = waiter;
        rwfd_->loop()
                .post<RWFdCrossThreadWaiter, &RWFdCrossThreadWaiter::notify_entry_,
                      &RWFdCrossThreadWaiter::on_notify_watch>(*waiter);
        return true;
    }

    fiber::common::IoResult<void> await_resume() noexcept {
        waiting_ = false;
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
    bool waiting_ = false;
    bool completed_ = false;
    RWFdCrossThreadWaiter *waiter_ = nullptr;
};

} // namespace fiber::net::detail

#endif // FIBER_NET_DETAIL_RW_FD_H
