#ifndef FIBER_NET_DETAIL_ACCEPT_FD_H
#define FIBER_NET_DETAIL_ACCEPT_FD_H

#include <cerrno>
#include <coroutine>
#include <unistd.h>
#include <utility>

#include "../../common/Assert.h"
#include "../../common/IoError.h"
#include "../../common/NonCopyable.h"
#include "../../common/NonMovable.h"
#include "../../event/EventLoop.h"
#include "Efd.h"

namespace fiber::net::detail {

template<typename Traits>
class AcceptFd : public common::NonCopyable, public common::NonMovable {
public:
    using Address = typename Traits::Address;
    using ListenOptions = typename Traits::ListenOptions;
    using AcceptResult = typename Traits::AcceptResult;

    class AcceptAwaiter;

    explicit AcceptFd(fiber::event::EventLoop &loop) :
        efd_(loop, this, &AcceptFd::on_efd_events, fiber::event::Poller::Mode::Edge) {}

    ~AcceptFd() {
        if (!efd_.valid()) {
            return;
        }
        if (!close_requires_loop() || efd_.loop().in_loop()) {
            close();
            return;
        }
        FIBER_ASSERT(false);
    }

    fiber::common::IoResult<void> bind(const Address &addr, const ListenOptions &options) {
        if (efd_.valid()) {
            return std::unexpected(fiber::common::IoErr::Already);
        }
        auto fd_result = Traits::bind(addr, options);
        if (!fd_result) {
            return std::unexpected(fd_result.error());
        }
        fiber::common::IoErr attach_err = efd_.attach(*fd_result);
        if (attach_err != fiber::common::IoErr::None) {
            ::close(*fd_result);
            return std::unexpected(attach_err);
        }
        return {};
    }

    [[nodiscard]] bool valid() const noexcept { return efd_.valid(); }

    [[nodiscard]] int fd() const noexcept { return efd_.fd(); }
    [[nodiscard]] fiber::event::EventLoop &loop() const noexcept { return efd_.loop(); }

    void close() {
        FIBER_ASSERT(!close_requires_loop() || efd_.loop().in_loop());
        if (!efd_.valid()) {
            return;
        }
        cancel_pending();
        if (stop_entry_.is_registered()) {
            loop().template unregister_stop<AcceptFd, &AcceptFd::stop_entry_>(*this);
        }
        auto *waiter = waiter_;
        waiter_ = nullptr;
        std::coroutine_handle<> handle{};
        if (waiter) {
            waiter->result_ = std::unexpected(fiber::common::IoErr::Canceled);
            waiter->waiting_ = false;
            handle = waiter->handle_;
            waiter->handle_ = {};
        }
        efd_.close_fd();
        if (handle) {
            handle.resume();
        }
    }

    [[nodiscard]] AcceptAwaiter accept() noexcept { return AcceptAwaiter(*this); }

private:
    friend class AcceptAwaiter;

    [[nodiscard]] bool close_requires_loop() const noexcept { return efd_.registered() || waiter_ != nullptr; }

    bool begin_wait(AcceptAwaiter *awaiter) {
        FIBER_ASSERT(efd_.loop().in_loop());
        if (!awaiter) {
            return false;
        }
        awaiter->result_ = AcceptResult{};
        if (!efd_.valid()) {
            awaiter->result_ = std::unexpected(fiber::common::IoErr::BadFd);
            return false;
        }
        if (waiter_) {
            awaiter->result_ = std::unexpected(fiber::common::IoErr::Busy);
            return false;
        }
        const auto watch_err = watch_read();
        if (watch_err != fiber::common::IoErr::None) {
            awaiter->result_ = std::unexpected(watch_err);
            return false;
        }
        if (!stop_entry_.is_registered() &&
            !loop().template register_stop<AcceptFd, &AcceptFd::stop_entry_, &AcceptFd::on_stop>(*this)) {
            awaiter->result_ = std::unexpected(fiber::common::IoErr::Canceled);
            return false;
        }
        if (loop().now() < retry_after_ || consecutive_ >= 64) {
            waiter_ = awaiter;
            awaiter->waiting_ = true;
            if (loop().now() < retry_after_) {
                loop().template post_at<AcceptFd, &AcceptFd::retry_entry_, &AcceptFd::on_retry>(retry_after_, *this);
            } else {
                loop().template post_local<AcceptFd, &AcceptFd::continue_entry_, &AcceptFd::on_retry>(*this);
            }
            return true;
        }
        if (ready_) {
            AcceptResult out;
            const auto err = Traits::accept_once(efd_.fd(), out);
            ++consecutive_;
            if (err == fiber::common::IoErr::None) {
                awaiter->result_ = std::move(out);
                return false;
            }
            if (err != fiber::common::IoErr::WouldBlock) {
                retry_after_ = loop().now() + std::chrono::milliseconds(1);
                awaiter->result_ = std::unexpected(err);
                return false;
            }
            ready_ = false;
        }
        waiter_ = awaiter;
        awaiter->waiting_ = true;
        return true;
    }

    void cancel_pending() noexcept {
        if (continue_entry_.is_in_queue()) {
            loop().template cancel<AcceptFd, &AcceptFd::continue_entry_>(*this);
        }
        if (retry_entry_.is_in_heap()) {
            loop().template cancel<AcceptFd, &AcceptFd::retry_entry_>(*this);
        }
    }

    void cancel_wait(AcceptAwaiter *awaiter) {
        FIBER_ASSERT(efd_.loop().in_loop());
        FIBER_ASSERT(awaiter == waiter_);
        waiter_ = nullptr;
        awaiter->waiting_ = false;
        cancel_pending();
    }

    fiber::common::IoErr watch_read() { return efd_.watch_set(fiber::event::IoEvent::Read); }

    static void on_stop(AcceptFd *owner) noexcept { owner->close(); }
    static void on_retry(AcceptFd *owner) noexcept {
        owner->consecutive_ = 0;
        owner->handle_acceptable();
    }

    void handle_acceptable() {
        if (!waiter_ || loop().now() < retry_after_) {
            return;
        }
        if (!ready_) {
            return;
        }
        AcceptResult out;
        const auto err = Traits::accept_once(efd_.fd(), out);
        if (err == fiber::common::IoErr::WouldBlock) {
            ready_ = false;
            return;
        }
        if (err != fiber::common::IoErr::None) {
            retry_after_ = loop().now() + std::chrono::milliseconds(1);
        }
        AcceptAwaiter *waiter = waiter_;
        waiter_ = nullptr;
        waiter->waiting_ = false;
        cancel_pending();
        if (err == fiber::common::IoErr::None) {
            waiter->result_ = std::move(out);
        } else {
            waiter->result_ = std::unexpected(err);
        }
        waiter->handle_.resume();
    }

    static void on_efd_events(void *owner, fiber::event::IoEvent events) {
        if (!owner) {
            return;
        }
        if (!fiber::event::any(events & fiber::event::IoEvent::Read)) {
            return;
        }
        auto *acceptor = static_cast<AcceptFd *>(owner);
        acceptor->ready_ = true;
        acceptor->consecutive_ = 0;
        acceptor->handle_acceptable();
    }

    Efd efd_;
    AcceptAwaiter *waiter_ = nullptr;
    bool ready_ = true;
    unsigned consecutive_ = 0;
    std::chrono::steady_clock::time_point retry_after_{};
    fiber::event::EventLoop::DeferEntry continue_entry_{};
    fiber::event::EventLoop::TimerEntry retry_entry_{};
    fiber::event::EventLoop::StopEntry stop_entry_{};
};

template<typename Traits>
class AcceptFd<Traits>::AcceptAwaiter {
public:
    explicit AcceptAwaiter(AcceptFd &acceptor) noexcept : acceptor_(&acceptor) {}

    AcceptAwaiter(const AcceptAwaiter &) = delete;
    AcceptAwaiter &operator=(const AcceptAwaiter &) = delete;
    AcceptAwaiter(AcceptAwaiter &&) = delete;
    AcceptAwaiter &operator=(AcceptAwaiter &&) = delete;

    ~AcceptAwaiter() {
        if (!waiting_) {
            return;
        }
        FIBER_ASSERT(acceptor_->efd_.loop().in_loop());
        acceptor_->cancel_wait(this);
    }

    bool await_ready() noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle) {
        handle_ = handle;
        return acceptor_->begin_wait(this);
    }

    fiber::common::IoResult<AcceptResult> await_resume() noexcept { return std::move(result_); }

private:
    friend class AcceptFd;

    AcceptFd *acceptor_ = nullptr;
    std::coroutine_handle<> handle_{};
    fiber::common::IoResult<AcceptResult> result_{};
    bool waiting_ = false;
};

} // namespace fiber::net::detail

#endif // FIBER_NET_DETAIL_ACCEPT_FD_H
