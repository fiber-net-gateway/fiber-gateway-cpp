#include "RWFd.h"

namespace fiber::net::detail {

namespace {

bool is_waiter_callback(RWFd::ReadyCallback callback) noexcept { return callback == &RWFdWaiterBase::on_ready; }

} // namespace

void RWFdWaiterBase::on_ready(void *ctx) noexcept {
    auto *waiter = static_cast<RWFdWaiterBase *>(ctx);
    FIBER_ASSERT(waiter != nullptr);
    waiter->complete(fiber::common::IoErr::None);
}

void RWFdWaiterBase::complete(fiber::common::IoErr err) noexcept {
    FIBER_ASSERT(complete_callback_ != nullptr);
    complete_callback_(this, err);
}

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
    FIBER_ASSERT(!has_callbacks());
    if (efd_.registered()) {
        FIBER_ASSERT(loop().in_loop());
        fiber::common::IoErr err = efd_.unwatch_all();
        FIBER_ASSERT(err == fiber::common::IoErr::None);
    }
    return efd_.release_fd();
}

void RWFd::close() {
    FIBER_ASSERT(loop().in_loop());
    if (!valid()) {
        return;
    }

    (void) efd_.unwatch_all();

    ReadyCallback read_callback = read_callback_;
    void *read_callback_ctx = read_callback_ctx_;
    ReadyCallback write_callback = write_callback_;
    void *write_callback_ctx = write_callback_ctx_;
    read_callback_ = nullptr;
    read_callback_ctx_ = nullptr;
    write_callback_ = nullptr;
    write_callback_ctx_ = nullptr;
    efd_.close_fd();

    auto cancel_waiter = [](ReadyCallback callback, void *ctx) noexcept {
        if (!is_waiter_callback(callback)) {
            return;
        }
        auto *waiter = static_cast<RWFdWaiterBase *>(ctx);
        FIBER_ASSERT(waiter != nullptr);
        waiter->complete(fiber::common::IoErr::Canceled);
    };
    cancel_waiter(read_callback, read_callback_ctx);
    cancel_waiter(write_callback, write_callback_ctx);
}

fiber::common::IoErr RWFd::set_read_callback(ReadyCallback callback, void *ctx) noexcept {
    FIBER_ASSERT(loop().in_loop());
    if (!callback) {
        return fiber::common::IoErr::Invalid;
    }
    if (read_callback_) {
        return is_waiter_callback(read_callback_) ? fiber::common::IoErr::Busy : fiber::common::IoErr::Already;
    }
    read_callback_ = callback;
    read_callback_ctx_ = ctx;
    fiber::common::IoErr err = sync_interest();
    if (err != fiber::common::IoErr::None) {
        read_callback_ = nullptr;
        read_callback_ctx_ = nullptr;
    }
    return err;
}

fiber::common::IoErr RWFd::set_write_callback(ReadyCallback callback, void *ctx) noexcept {
    FIBER_ASSERT(loop().in_loop());
    if (!callback) {
        return fiber::common::IoErr::Invalid;
    }
    if (write_callback_) {
        return is_waiter_callback(write_callback_) ? fiber::common::IoErr::Busy : fiber::common::IoErr::Already;
    }
    write_callback_ = callback;
    write_callback_ctx_ = ctx;
    fiber::common::IoErr err = sync_interest();
    if (err != fiber::common::IoErr::None) {
        write_callback_ = nullptr;
        write_callback_ctx_ = nullptr;
    }
    return err;
}

fiber::common::IoErr RWFd::clear_read_callback() noexcept {
    FIBER_ASSERT(loop().in_loop());
    if (!read_callback_ || is_waiter_callback(read_callback_)) {
        return fiber::common::IoErr::None;
    }
    ReadyCallback callback = read_callback_;
    void *ctx = read_callback_ctx_;
    read_callback_ = nullptr;
    read_callback_ctx_ = nullptr;
    fiber::common::IoErr err = sync_interest();
    if (err != fiber::common::IoErr::None) {
        read_callback_ = callback;
        read_callback_ctx_ = ctx;
    }
    return err;
}

fiber::common::IoErr RWFd::clear_write_callback() noexcept {
    FIBER_ASSERT(loop().in_loop());
    if (!write_callback_ || is_waiter_callback(write_callback_)) {
        return fiber::common::IoErr::None;
    }
    ReadyCallback callback = write_callback_;
    void *ctx = write_callback_ctx_;
    write_callback_ = nullptr;
    write_callback_ctx_ = nullptr;
    fiber::common::IoErr err = sync_interest();
    if (err != fiber::common::IoErr::None) {
        write_callback_ = callback;
        write_callback_ctx_ = ctx;
    }
    return err;
}

RWFd::WaitReadableAwaiter RWFd::wait_readable(std::chrono::milliseconds timeout) noexcept {
    return WaitReadableAwaiter(*this, timeout);
}

RWFd::WaitWritableAwaiter RWFd::wait_writable(std::chrono::milliseconds timeout) noexcept {
    return WaitWritableAwaiter(*this, timeout);
}

fiber::common::IoErr RWFd::begin_wait(RWFdWaiterBase *waiter) noexcept {
    FIBER_ASSERT(loop().in_loop());
    FIBER_ASSERT(waiter != nullptr);
    FIBER_ASSERT(waiter->event_ == fiber::event::IoEvent::Read || waiter->event_ == fiber::event::IoEvent::Write);
    if (!valid()) {
        return fiber::common::IoErr::BadFd;
    }

    ReadyCallback *callback_slot = nullptr;
    void **ctx_slot = nullptr;
    if (waiter->event_ == fiber::event::IoEvent::Read) {
        callback_slot = &read_callback_;
        ctx_slot = &read_callback_ctx_;
    } else {
        callback_slot = &write_callback_;
        ctx_slot = &write_callback_ctx_;
    }
    if (*callback_slot) {
        return fiber::common::IoErr::Busy;
    }

    *callback_slot = &RWFdWaiterBase::on_ready;
    *ctx_slot = waiter;
    fiber::common::IoErr err = sync_interest();
    if (err != fiber::common::IoErr::None) {
        *callback_slot = nullptr;
        *ctx_slot = nullptr;
    }
    return err;
}

fiber::common::IoErr RWFd::cancel_wait(RWFdWaiterBase *waiter) noexcept {
    FIBER_ASSERT(loop().in_loop());
    FIBER_ASSERT(waiter != nullptr);

    bool removed = false;
    if (read_callback_ == &RWFdWaiterBase::on_ready && read_callback_ctx_ == waiter) {
        read_callback_ = nullptr;
        read_callback_ctx_ = nullptr;
        removed = true;
    }
    if (write_callback_ == &RWFdWaiterBase::on_ready && write_callback_ctx_ == waiter) {
        write_callback_ = nullptr;
        write_callback_ctx_ = nullptr;
        removed = true;
    }
    if (!removed) {
        return fiber::common::IoErr::None;
    }
    return sync_interest();
}

void RWFd::on_efd_events(void *owner, fiber::event::IoEvent events) {
    auto *rwfd = static_cast<RWFd *>(owner);
    if (!rwfd) {
        return;
    }
    rwfd->handle_events(events);
}

void RWFd::handle_events(fiber::event::IoEvent events) {
    FIBER_ASSERT(loop().in_loop());
    if (!fiber::event::any(events)) {
        return;
    }

    ReadyCallback read_callback = nullptr;
    void *read_callback_ctx = nullptr;
    ReadyCallback write_callback = nullptr;
    void *write_callback_ctx = nullptr;

    if (fiber::event::any(events & fiber::event::IoEvent::Read)) {
        read_callback = read_callback_;
        read_callback_ctx = read_callback_ctx_;
    }
    if (fiber::event::any(events & fiber::event::IoEvent::Write)) {
        write_callback = write_callback_;
        write_callback_ctx = write_callback_ctx_;
    }

    fiber::event::IoEvent old_watching = efd_.watching();
    fiber::common::IoErr consume_err = efd_.consume_ready(old_watching);
    FIBER_ASSERT(consume_err == fiber::common::IoErr::None);

    if (read_callback && !is_waiter_callback(read_callback)) {
        read_callback(read_callback_ctx);
        if (!valid()) {
            return;
        }
    }
    if (write_callback && !is_waiter_callback(write_callback) && write_callback_ == write_callback &&
        write_callback_ctx_ == write_callback_ctx) {
        write_callback(write_callback_ctx);
        if (!valid()) {
            return;
        }
    }

    bool notify_read_waiter = false;
    bool notify_write_waiter = false;
    if (is_waiter_callback(read_callback) && read_callback_ == read_callback &&
        read_callback_ctx_ == read_callback_ctx) {
        read_callback_ = nullptr;
        read_callback_ctx_ = nullptr;
        notify_read_waiter = true;
    }
    if (is_waiter_callback(write_callback) && write_callback_ == write_callback &&
        write_callback_ctx_ == write_callback_ctx) {
        write_callback_ = nullptr;
        write_callback_ctx_ = nullptr;
        notify_write_waiter = true;
    }

    fiber::common::IoErr rearm_err = fiber::common::IoErr::None;
    fiber::event::IoEvent active = active_events();
    if (fiber::event::any(active)) {
        rearm_err = efd_.watch_set(active);
    }
    FIBER_ASSERT(rearm_err == fiber::common::IoErr::None);

    if (notify_read_waiter) {
        auto *waiter = static_cast<RWFdWaiterBase *>(read_callback_ctx);
        waiter->complete(rearm_err);
    }
    if (notify_write_waiter) {
        auto *waiter = static_cast<RWFdWaiterBase *>(write_callback_ctx);
        waiter->complete(rearm_err);
    }
}

bool RWFd::has_callbacks() const noexcept { return read_callback_ != nullptr || write_callback_ != nullptr; }

fiber::event::IoEvent RWFd::active_events() const noexcept {
    fiber::event::IoEvent events = fiber::event::IoEvent::None;
    if (read_callback_) {
        events |= fiber::event::IoEvent::Read;
    }
    if (write_callback_) {
        events |= fiber::event::IoEvent::Write;
    }
    return events;
}

fiber::common::IoErr RWFd::sync_interest() noexcept { return efd_.watch_set(active_events()); }

void RWFdCrossThreadWaiter::on_complete(RWFdWaiterBase *base, fiber::common::IoErr err) noexcept {
    auto *waiter = static_cast<RWFdCrossThreadWaiter *>(base);
    waiter->err_ = err;
    do_notify_resume(waiter);
}

void RWFdCrossThreadWaiter::on_notify_cancel(RWFdCrossThreadWaiter *waiter) noexcept {
    RWFdWaiterState state = waiter->state_.load(std::memory_order_relaxed);
    RWFd *rwfd = waiter->rwfd_;
    FIBER_ASSERT(rwfd->loop().in_loop());
    if (state == RWFdWaiterState::Request_Cancel) {
        (void) rwfd->cancel_wait(waiter);
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
    fiber::common::IoErr err = rwfd->begin_wait(waiter);
    if (err != fiber::common::IoErr::None) {
        waiter->err_ = err;
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
