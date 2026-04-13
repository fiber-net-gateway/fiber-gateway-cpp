#include "RWFd.h"

namespace fiber::net::detail {

RWFd::RWFd(fiber::event::EventLoop &loop) :
    efd_(loop, this, &RWFd::on_efd_events, fiber::event::Poller::Mode::OneShot) {}

RWFd::RWFd(fiber::event::EventLoop &loop, int fd) :
    efd_(loop, this, &RWFd::on_efd_events, fiber::event::Poller::Mode::OneShot) {
    fiber::common::IoErr err = efd_.attach(fd);
    FIBER_ASSERT(err == fiber::common::IoErr::None);
}

RWFd::~RWFd() {
    if (!valid()) {
        return;
    }
    if (loop().in_loop()) {
        close();
        return;
    }
    FIBER_ASSERT(false);
}

bool RWFd::valid() const noexcept { return efd_.valid(); }

int RWFd::fd() const noexcept { return efd_.fd(); }

fiber::event::EventLoop &RWFd::loop() const noexcept { return efd_.loop(); }

fiber::common::IoErr RWFd::attach(int fd) noexcept { return efd_.attach(fd); }

int RWFd::release_fd() noexcept {
    FIBER_ASSERT(!has_waiters());
    return efd_.release_fd();
}

void RWFd::close() {
    FIBER_ASSERT(loop().in_loop());
    if (!valid()) {
        return;
    }

    (void) efd_.unwatch_all();

    if (waiter_ != nullptr) {
        if (local_waiting_) {
            auto *waiter = local_waiter_;
            local_waiter_ = nullptr;
            waiter->err_ = fiber::common::IoErr::Canceled;
            waiter->ready_ = fiber::event::IoEvent::None;
            waiter->coro_.resume();
        } else {
            auto *waiter = cross_waiter_;
            cross_waiter_ = nullptr;
            waiter->err_ = fiber::common::IoErr::Canceled;
            waiter->ready_ = fiber::event::IoEvent::None;
            RWFdCrossThreadWaiter::do_notify_resume(waiter);
        }
    }
    local_waiting_ = false;

    efd_.close_fd();
}

RWFd::WaitEventAwaiter RWFd::wait_event(fiber::event::IoEvent interested) noexcept {
    return WaitEventAwaiter(*this, interested);
}

RWFd::WaitReadableAwaiter RWFd::wait_readable() noexcept { return WaitReadableAwaiter(*this); }

RWFd::WaitWritableAwaiter RWFd::wait_writable() noexcept { return WaitWritableAwaiter(*this); }

void RWFd::on_efd_events(void *owner, fiber::event::IoEvent events) {
    auto *rwfd = static_cast<RWFd *>(owner);
    if (!rwfd) {
        return;
    }
    rwfd->handle_events(events);
}

void RWFd::handle_events(fiber::event::IoEvent events) {
    FIBER_ASSERT(loop().in_loop());
    if (!fiber::event::any(events) || waiter_ == nullptr) {
        return;
    }

    auto *waiter = waiter_base_;
    bool was_local = local_waiting_;
    fiber::event::IoEvent ready = events & waiter->interested_;
    if (!fiber::event::any(ready)) {
        return;
    }

    (void) efd_.consume_ready(ready);

    waiter_ = nullptr;
    local_waiting_ = false;
    waiter->ready_ = ready;
    waiter->err_ = fiber::common::IoErr::None;

    if (was_local) {
        static_cast<RWFdLocalThreadWaiter *>(waiter)->coro_.resume();
        return;
    }

    RWFdCrossThreadWaiter::do_notify_resume(static_cast<RWFdCrossThreadWaiter *>(waiter));
}

bool RWFd::has_waiters() const noexcept { return waiter_ != nullptr; }

void RWFdCrossThreadWaiter::on_notify_cancel(RWFdCrossThreadWaiter *waiter) {
    RWFdWaiterState state = waiter->state_.load(std::memory_order_relaxed);
    RWFd *rwfd = waiter->rwfd_;
    FIBER_ASSERT(rwfd->loop().in_loop());
    if (state == RWFdWaiterState::Request_Cancel) {
        (void) rwfd->cancel_wait<RWFdCrossThreadWaiter>(waiter);
    } else {
        FIBER_ASSERT(state == RWFdWaiterState::Waiting_Cancel);
    }
    delete waiter;
}

void RWFdCrossThreadWaiter::cancel_wait() noexcept {
    RWFdWaiterState state = state_.load(std::memory_order_acquire);
    RWFdWaiterState expected;
    for (;;) {
        switch (state) {
            case RWFdWaiterState::Notify_Watch:
            case RWFdWaiterState::Notify_Resume:
                expected = RWFdWaiterState::Canceled;
                break;
            case RWFdWaiterState::Watching_Event:
                expected = RWFdWaiterState::Request_Cancel;
                break;
            case RWFdWaiterState::Request_Cancel:
            case RWFdWaiterState::Waiting_Cancel:
            case RWFdWaiterState::Canceled:
                return;
            default:
                return;
        }
        if (state_.compare_exchange_weak(state, expected, std::memory_order_acq_rel, std::memory_order_acquire)) {
            break;
        }
    }

    if (expected == RWFdWaiterState::Request_Cancel) {
        rwfd_->loop()
                .post<RWFdCrossThreadWaiter, &RWFdCrossThreadWaiter::cancel_entry_,
                      &RWFdCrossThreadWaiter::on_notify_cancel>(*this);
    }
}

void RWFdCrossThreadWaiter::do_notify_resume(RWFdCrossThreadWaiter *waiter) noexcept {
    RWFdWaiterState state = waiter->state_.load(std::memory_order_acquire);
    RWFdWaiterState expected;

    for (;;) {
        switch (state) {
            case RWFdWaiterState::Watching_Event:
                expected = RWFdWaiterState::Notify_Resume;
                break;
            case RWFdWaiterState::Request_Cancel:
                expected = RWFdWaiterState::Waiting_Cancel;
                break;
            case RWFdWaiterState::Notify_Resume:
            case RWFdWaiterState::Waiting_Cancel:
            case RWFdWaiterState::Canceled:
                return;
            default:
                return;
        }
        if (waiter->state_.compare_exchange_weak(state, expected, std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
            break;
        }
    }

    if (expected == RWFdWaiterState::Notify_Resume) {
        waiter->loop_->post<RWFdCrossThreadWaiter, &RWFdCrossThreadWaiter::cancel_entry_,
                            &RWFdCrossThreadWaiter::on_notify_resume>(*waiter);
    }
}

void RWFdCrossThreadWaiter::on_notify_watch(RWFdCrossThreadWaiter *waiter) {
    FIBER_ASSERT(waiter);
    FIBER_ASSERT(waiter->rwfd_);

    RWFdWaiterState old = waiter->state_.exchange(RWFdWaiterState::Watching_Event, std::memory_order_acq_rel);
    if (old == RWFdWaiterState::Canceled) {
        delete waiter;
        return;
    }
    FIBER_ASSERT(old == RWFdWaiterState::Notify_Watch);

    RWFd *rwfd = waiter->rwfd_;
    fiber::common::IoErr err = rwfd->begin_wait<RWFdCrossThreadWaiter>(waiter);
    if (err != fiber::common::IoErr::None) {
        waiter->err_ = err;
        waiter->ready_ = fiber::event::IoEvent::None;
        do_notify_resume(waiter);
    }
}

void RWFdCrossThreadWaiter::on_notify_resume(RWFdCrossThreadWaiter *waiter) {
    FIBER_ASSERT(waiter);
    FIBER_ASSERT(waiter->loop_->in_loop());

    if (waiter->state_.load(std::memory_order_relaxed) == RWFdWaiterState::Canceled) {
        delete waiter;
        return;
    }

    waiter->coro_.resume();
}

template class RWFd::WaitAwaiter<fiber::event::IoEvent::Read>;
template class RWFd::WaitAwaiter<fiber::event::IoEvent::Write>;

} // namespace fiber::net::detail
