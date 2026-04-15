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

    RWFdWaiterBase *waiters[2] = {read_waiter_, write_waiter_};
    bool was_local[2] = {read_waiter_local_, write_waiter_local_};
    read_waiter_ = nullptr;
    write_waiter_ = nullptr;
    read_waiter_local_ = false;
    write_waiter_local_ = false;

    for (std::size_t i = 0; i < 2; ++i) {
        RWFdWaiterBase *waiter = waiters[i];
        if (!waiter) {
            continue;
        }
        if (i != 0 && waiter == waiters[0]) {
            continue;
        }
        waiter->err_ = fiber::common::IoErr::Canceled;
        waiter->ready_ = fiber::event::IoEvent::None;
        if (was_local[i]) {
            static_cast<RWFdLocalThreadWaiter *>(waiter)->coro_.resume();
        } else {
            RWFdCrossThreadWaiter::do_notify_resume(static_cast<RWFdCrossThreadWaiter *>(waiter));
        }
    }

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
    if (!fiber::event::any(events) || !has_waiters()) {
        return;
    }

    RWFdWaiterBase *waiters[2] = {read_waiter_, write_waiter_};
    bool was_local[2] = {read_waiter_local_, write_waiter_local_};
    std::size_t count = 0;
    fiber::event::IoEvent consumed = fiber::event::IoEvent::None;

    auto collect_waiter = [&](RWFdWaiterBase *waiter, bool local) noexcept {
        if (!waiter) {
            return;
        }
        for (std::size_t i = 0; i < count; ++i) {
            if (waiters[i] == waiter) {
                return;
            }
        }
        fiber::event::IoEvent ready = events & waiter->interested_;
        if (!fiber::event::any(ready)) {
            return;
        }
        waiter->ready_ = ready;
        waiter->err_ = fiber::common::IoErr::None;
        waiters[count] = waiter;
        was_local[count] = local;
        ++count;
        consumed |= waiter->interested_;
    };

    collect_waiter(read_waiter_, read_waiter_local_);
    collect_waiter(write_waiter_, write_waiter_local_);
    if (count == 0) {
        return;
    }

    for (std::size_t i = 0; i < count; ++i) {
        if (read_waiter_ == waiters[i]) {
            read_waiter_ = nullptr;
            read_waiter_local_ = false;
        }
        if (write_waiter_ == waiters[i]) {
            write_waiter_ = nullptr;
            write_waiter_local_ = false;
        }
    }

    (void) efd_.consume_ready(consumed);

    for (std::size_t i = 0; i < count; ++i) {
        if (was_local[i]) {
            static_cast<RWFdLocalThreadWaiter *>(waiters[i])->coro_.resume();
        } else {
            RWFdCrossThreadWaiter::do_notify_resume(static_cast<RWFdCrossThreadWaiter *>(waiters[i]));
        }
    }
}

bool RWFd::has_waiters() const noexcept { return read_waiter_ != nullptr || write_waiter_ != nullptr; }

fiber::event::IoEvent RWFd::waiting_events() const noexcept {
    fiber::event::IoEvent events = fiber::event::IoEvent::None;
    if (read_waiter_ != nullptr) {
        events |= fiber::event::IoEvent::Read;
    }
    if (write_waiter_ != nullptr) {
        events |= fiber::event::IoEvent::Write;
    }
    return events;
}

void RWFdCrossThreadWaiter::on_notify_cancel(RWFdCrossThreadWaiter *waiter) noexcept {
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

void RWFdCrossThreadWaiter::on_notify_watch(RWFdCrossThreadWaiter *waiter) noexcept {
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

void RWFdCrossThreadWaiter::on_notify_resume(RWFdCrossThreadWaiter *waiter) noexcept {
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
