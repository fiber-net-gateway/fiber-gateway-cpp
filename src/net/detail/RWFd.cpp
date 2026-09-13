#include <fiber/net/detail/RWFd.h>

#include <cerrno>
#include <poll.h>
#include <utility>

namespace fiber::net::detail {

RWFd::DispatchGuard::DispatchGuard(RWFd &owner) noexcept : owner_(&owner) {
    FIBER_ASSERT_MSG(owner.dispatch_destroyed_observer_ == nullptr, "RWFd event dispatch cannot re-enter");
    owner.dispatch_destroyed_observer_ = &owner_destroyed_;
}

RWFd::DispatchGuard::~DispatchGuard() noexcept {
    if (owner_destroyed_) {
        return;
    }
    FIBER_ASSERT(owner_ != nullptr);
    FIBER_ASSERT(owner_->dispatch_destroyed_observer_ == &owner_destroyed_);
    owner_->dispatch_destroyed_observer_ = nullptr;
}

void RWFdWaiterBase::on_event(void *ctx, fiber::common::IoErr err) noexcept {
    auto *waiter = static_cast<RWFdWaiterBase *>(ctx);
    FIBER_ASSERT(waiter != nullptr);
    fiber::common::IoErr clear_err = waiter->rwfd_->cancel_wait(waiter);
    if (err == fiber::common::IoErr::None && clear_err != fiber::common::IoErr::None) {
        err = clear_err;
    }
    waiter->complete(err);
}

void RWFdWaiterBase::complete(fiber::common::IoErr err) noexcept {
    FIBER_ASSERT(complete_callback_ != nullptr);
    complete_callback_(this, err);
}

RWFd::RWFd(event::EventLoop &loop, Kind kind) :
    kind_(kind), external_io_(kind == Kind::Raw), efd_(loop, this, &RWFd::on_efd_events, event::Poller::Mode::Edge) {}

RWFd::RWFd(event::EventLoop &loop, int socket, Kind kind) : RWFd(loop, kind) {
    const auto err = attach(socket);
    FIBER_ASSERT(err == common::IoErr::None);
}

RWFd::~RWFd() {
    if (dispatch_destroyed_observer_) {
        *dispatch_destroyed_observer_ = true;
        dispatch_destroyed_observer_ = nullptr;
    }
    if (loop().in_loop()) {
        close();
    } else {
        FIBER_ASSERT(!efd_.registered() && !has_callbacks() && !ready_entry_.is_in_queue());
    }
}

bool RWFd::valid() const noexcept { return efd_.valid(); }
int RWFd::fd() const noexcept { return efd_.fd(); }
event::EventLoop &RWFd::loop() const noexcept { return efd_.loop(); }

common::IoErr RWFd::attach(int socket) noexcept {
    const auto err = efd_.attach(socket);
    if (err == common::IoErr::None) {
        terminal_ = false;
        terminal_error_ = common::IoErr::None;
        read_ready_ = write_ready_ = Readiness::Unknown;
        read_hangup_ = false;
        external_io_.store(kind_ == Kind::Raw, std::memory_order_release);
    }
    return err;
}

int RWFd::release_fd() noexcept {
    FIBER_ASSERT(!has_callbacks());
    if (efd_.registered() && efd_.unwatch_all() != common::IoErr::None) {
        return -1;
    }
    if (ready_entry_.is_in_queue()) {
        loop().cancel<RWFd, &RWFd::ready_entry_>(*this);
    }
    if (stop_entry_.is_registered()) {
        loop().unregister_stop<RWFd, &RWFd::stop_entry_>(*this);
    }
    pending_ready_ = event::IoEvent::None;
    return efd_.release_fd();
}

void RWFd::on_loop_stop(RWFd *owner) noexcept { owner->close(); }

common::IoErr RWFd::ensure_registered() noexcept {
    FIBER_ASSERT(loop().in_loop());
    event::IoEvent interest = event::IoEvent::Read | event::IoEvent::Write | event::IoEvent::Terminal;
    if (kind_ != Kind::Datagram) {
        interest |= event::IoEvent::ReadHangup;
    }
    const auto err = efd_.watch_set(interest);
    if (err == common::IoErr::None && !stop_entry_.is_registered()) {
        if (!loop().register_stop<RWFd, &RWFd::stop_entry_, &RWFd::on_loop_stop>(*this)) {
            return common::IoErr::Canceled;
        }
    }
    return err;
}

common::IoErr RWFd::prepare_io(event::IoEvent direction) noexcept {
    if (!valid()) {
        return common::IoErr::BadFd;
    }
    if (!loop().in_loop()) {
        use_external_io();
        return common::IoErr::None;
    }
    const auto err = ensure_registered();
    if (err != common::IoErr::None) {
        return err;
    }
    if (!external_io_.load(std::memory_order_acquire)) {
        const auto state = direction == event::IoEvent::Read ? read_ready_ : write_ready_;
        if (state == Readiness::Blocked && !terminal_) {
            return common::IoErr::WouldBlock;
        }
    }
    return common::IoErr::None;
}

void RWFd::finish_io(event::IoEvent direction, common::IoErr err, bool exhausted) noexcept {
    if (!loop().in_loop()) {
        return;
    }
    auto &state = direction == event::IoEvent::Read ? read_ready_ : write_ready_;
    if (err == common::IoErr::WouldBlock) {
        state = Readiness::Blocked;
    } else if (err == common::IoErr::None) {
        const bool drained = direction == event::IoEvent::Read && exhausted && !read_hangup_ && !terminal_ &&
                             !external_io_.load(std::memory_order_acquire);
        state = drained ? Readiness::Blocked : Readiness::Ready;
    } else if (err == common::IoErr::ConnReset || err == common::IoErr::BrokenPipe ||
               err == common::IoErr::NotConnected || err == common::IoErr::ConnAborted) {
        mark_terminal(err);
    }
    if (state == Readiness::Blocked) {
        pending_ready_ &= ~direction;
    }
}

common::IoResult<bool> RWFd::check_ready(event::IoEvent direction) noexcept {
    if (direction == event::IoEvent::Terminal) {
        return terminal_;
    }
    if (external_io_.load(std::memory_order_acquire)) {
        pollfd descriptor{.fd = fd(),
                          .events = static_cast<short>(direction == event::IoEvent::Read ? POLLIN : POLLOUT)};
        int result;
        do {
            result = ::poll(&descriptor, 1, 0);
        } while (result < 0 && errno == EINTR);
        if (result < 0) {
            return std::unexpected(common::io_err_from_errno(errno));
        }
        if (descriptor.revents & POLLNVAL) {
            return std::unexpected(common::IoErr::BadFd);
        }
        return descriptor.revents != 0;
    }
    return terminal_ || (direction == event::IoEvent::Read ? read_ready_ : write_ready_) == Readiness::Ready;
}

void RWFd::close() {
    FIBER_ASSERT(loop().in_loop());
    if (ready_entry_.is_in_queue()) {
        loop().cancel<RWFd, &RWFd::ready_entry_>(*this);
    }
    if (stop_entry_.is_registered()) {
        loop().unregister_stop<RWFd, &RWFd::stop_entry_>(*this);
    }
    pending_ready_ = event::IoEvent::None;
    const auto read_callback = std::exchange(read_callback_, nullptr);
    void *read_ctx = std::exchange(read_callback_ctx_, nullptr);
    const auto write_callback = std::exchange(write_callback_, nullptr);
    void *write_ctx = std::exchange(write_callback_ctx_, nullptr);
    const auto terminal_callback = std::exchange(terminal_callback_, nullptr);
    void *terminal_ctx = std::exchange(terminal_callback_ctx_, nullptr);
    if (read_generation_ != UINT64_MAX) {
        ++read_generation_;
    }
    if (write_generation_ != UINT64_MAX) {
        ++write_generation_;
    }
    if (terminal_generation_ != UINT64_MAX) {
        ++terminal_generation_;
    }
    if (!terminal_) {
        terminal_ = true;
        terminal_error_ = common::IoErr::Canceled;
    }
    const auto error = terminal_error_;
    efd_.close_fd();
    // All state is detached before a completion may destroy this object.
    auto complete_closed = [](ReadyCallback callback, void *ctx) noexcept {
        if (callback == &RWFdWaiterBase::on_event) {
            // The waiter has already been detached. In particular, its RWFd
            // may have been destroyed by the preceding terminal completion.
            static_cast<RWFdWaiterBase *>(ctx)->complete(common::IoErr::Canceled);
        } else if (callback) {
            callback(ctx, common::IoErr::Canceled);
        }
    };
    if (terminal_callback) {
        terminal_callback(terminal_ctx, error);
    }
    complete_closed(read_callback, read_ctx);
    complete_closed(write_callback, write_ctx);
}

common::IoErr RWFd::install_callback(event::IoEvent direction, ReadyCallback callback, void *ctx) noexcept {
    FIBER_ASSERT(loop().in_loop());
    if (!callback) {
        return common::IoErr::Invalid;
    }
    auto &slot = direction == event::IoEvent::Read    ? read_callback_
                 : direction == event::IoEvent::Write ? write_callback_
                                                      : terminal_callback_;
    auto &context = direction == event::IoEvent::Read    ? read_callback_ctx_
                    : direction == event::IoEvent::Write ? write_callback_ctx_
                                                         : terminal_callback_ctx_;
    auto &generation = direction == event::IoEvent::Read    ? read_generation_
                       : direction == event::IoEvent::Write ? write_generation_
                                                            : terminal_generation_;
    if (slot) {
        return common::IoErr::Busy;
    }
    if (generation == UINT64_MAX) {
        return common::IoErr::Invalid;
    }
    const auto err = ensure_registered();
    if (err != common::IoErr::None) {
        return err;
    }
    auto ready = check_ready(direction);
    if (!ready) {
        return ready.error();
    }
    slot = callback;
    context = ctx;
    ++generation;
    if (*ready) {
        queue_ready(direction);
    }
    return common::IoErr::None;
}

common::IoErr RWFd::set_read_callback(ReadyCallback cb, void *ctx) noexcept {
    return install_callback(event::IoEvent::Read, cb, ctx);
}
common::IoErr RWFd::set_write_callback(ReadyCallback cb, void *ctx) noexcept {
    return install_callback(event::IoEvent::Write, cb, ctx);
}
common::IoErr RWFd::set_terminal_callback(ReadyCallback cb, void *ctx) noexcept {
    return install_callback(event::IoEvent::Terminal, cb, ctx);
}

bool RWFd::remove_callback(event::IoEvent direction, ReadyCallback callback, void *ctx) noexcept {
    auto &slot = direction == event::IoEvent::Read    ? read_callback_
                 : direction == event::IoEvent::Write ? write_callback_
                                                      : terminal_callback_;
    auto &context = direction == event::IoEvent::Read    ? read_callback_ctx_
                    : direction == event::IoEvent::Write ? write_callback_ctx_
                                                         : terminal_callback_ctx_;
    auto &generation = direction == event::IoEvent::Read    ? read_generation_
                       : direction == event::IoEvent::Write ? write_generation_
                                                            : terminal_generation_;
    if (slot != callback || context != ctx) {
        return false;
    }
    slot = nullptr;
    context = nullptr;
    // Saturation rejects later subscriptions instead of wrapping their identity.
    if (generation != UINT64_MAX) {
        ++generation;
    }
    pending_ready_ &= ~direction;
    if (!event::any(pending_ready_) && ready_entry_.is_in_queue()) {
        loop().cancel<RWFd, &RWFd::ready_entry_>(*this);
    }
    return true;
}

common::IoErr RWFd::clear_read_callback(ReadyCallback cb, void *ctx) noexcept {
    FIBER_ASSERT(loop().in_loop());
    if (!cb) {
        return common::IoErr::Invalid;
    }
    (void) remove_callback(event::IoEvent::Read, cb, ctx);
    return common::IoErr::None;
}
common::IoErr RWFd::clear_write_callback(ReadyCallback cb, void *ctx) noexcept {
    FIBER_ASSERT(loop().in_loop());
    if (!cb) {
        return common::IoErr::Invalid;
    }
    (void) remove_callback(event::IoEvent::Write, cb, ctx);
    return common::IoErr::None;
}
common::IoErr RWFd::clear_terminal_callback(ReadyCallback cb, void *ctx) noexcept {
    FIBER_ASSERT(loop().in_loop());
    if (!cb) {
        return common::IoErr::Invalid;
    }
    (void) remove_callback(event::IoEvent::Terminal, cb, ctx);
    return common::IoErr::None;
}

void RWFd::mark_terminal(common::IoErr error) noexcept {
    FIBER_ASSERT(loop().in_loop());
    if (terminal_) {
        return;
    }
    terminal_ = true;
    terminal_error_ = error == common::IoErr::None ? common::IoErr::Unknown : error;
    read_ready_ = write_ready_ = Readiness::Ready;
    queue_ready(active_events());
}

RWFd::WaitReadableAwaiter RWFd::wait_readable(std::chrono::milliseconds timeout) noexcept {
    return WaitReadableAwaiter(*this, timeout);
}
RWFd::WaitWritableAwaiter RWFd::wait_writable(std::chrono::milliseconds timeout) noexcept {
    return WaitWritableAwaiter(*this, timeout);
}

common::IoResult<bool> RWFd::begin_wait(RWFdWaiterBase *waiter) noexcept {
    FIBER_ASSERT(loop().in_loop());
    const auto direction = waiter->event_;
    if ((direction == event::IoEvent::Read ? read_callback_ : write_callback_)) {
        return std::unexpected(common::IoErr::Busy);
    }
    const auto err = ensure_registered();
    if (err != common::IoErr::None) {
        return std::unexpected(err);
    }
    auto ready = check_ready(direction);
    if (!ready) {
        return std::unexpected(ready.error());
    }
    if (*ready) {
        return false;
    }
    // install_callback cannot run the callback synchronously.
    const auto installed = install_callback(direction, &RWFdWaiterBase::on_event, waiter);
    if (installed != common::IoErr::None) {
        return std::unexpected(installed);
    }
    return true;
}

common::IoErr RWFd::cancel_wait(RWFdWaiterBase *waiter) noexcept {
    FIBER_ASSERT(loop().in_loop());
    (void) remove_callback(waiter->event_, &RWFdWaiterBase::on_event, waiter);
    return common::IoErr::None;
}

void RWFd::queue_ready(event::IoEvent events) noexcept {
    pending_ready_ |= events;
    if (event::any(pending_ready_)) {
        loop().post_local<RWFd, &RWFd::ready_entry_, &RWFd::on_deferred_ready>(*this);
    }
}

void RWFd::on_deferred_ready(RWFd *owner) noexcept {
    auto events = std::exchange(owner->pending_ready_, event::IoEvent::None);
    if (!owner->external_io_.load(std::memory_order_acquire)) {
        if (owner->read_ready_ == Readiness::Blocked) {
            events &= ~event::IoEvent::Read;
        }
        if (owner->write_ready_ == Readiness::Blocked) {
            events &= ~event::IoEvent::Write;
        }
    }
    owner->dispatch_ready(events);
}

void RWFd::on_efd_events(void *owner, event::IoEvent events) { static_cast<RWFd *>(owner)->handle_events(events); }

void RWFd::handle_events(event::IoEvent events) {
    FIBER_ASSERT(loop().in_loop());
    if (event::any(events & event::IoEvent::ReadHangup)) {
        read_hangup_ = true;
        events |= event::IoEvent::Read;
    }
    if (event::any(events & event::IoEvent::Terminal)) {
        if (!terminal_) {
            terminal_ = true;
            terminal_error_ = common::IoErr::Unknown;
        }
        events |= event::IoEvent::Read | event::IoEvent::Write;
    }
    if (event::any(events & event::IoEvent::Read)) {
        read_ready_ = Readiness::Ready;
    }
    if (event::any(events & event::IoEvent::Write)) {
        write_ready_ = Readiness::Ready;
    }
    pending_ready_ &= ~events;
    if (!event::any(pending_ready_) && ready_entry_.is_in_queue()) {
        loop().cancel<RWFd, &RWFd::ready_entry_>(*this);
    }
    dispatch_ready(events);
}

void RWFd::dispatch_ready(event::IoEvent events) {
    if (!event::any(events)) {
        return;
    }
    DispatchGuard guard(*this);
    const auto epoch = efd_.epoch();
    const auto read_generation = read_generation_;
    const auto write_generation = write_generation_;
    if (event::any(events & event::IoEvent::Terminal) && terminal_callback_) {
        auto callback = terminal_callback_;
        void *ctx = terminal_callback_ctx_;
        (void) remove_callback(event::IoEvent::Terminal, callback, ctx);
        callback(ctx, terminal_error_);
        if (guard.owner_destroyed() || !valid() || efd_.epoch() != epoch) {
            return;
        }
    }
    if (event::any(events & event::IoEvent::Read) && read_callback_ && read_generation == read_generation_) {
        read_callback_(read_callback_ctx_, common::IoErr::None);
        if (guard.owner_destroyed() || !valid() || efd_.epoch() != epoch) {
            return;
        }
    }
    if (event::any(events & event::IoEvent::Write) && write_callback_ && write_generation == write_generation_) {
        write_callback_(write_callback_ctx_, common::IoErr::None);
    }
}

bool RWFd::has_callbacks() const noexcept { return read_callback_ || write_callback_ || terminal_callback_; }

event::IoEvent RWFd::active_events() const noexcept {
    event::IoEvent events = event::IoEvent::None;
    if (read_callback_) {
        events |= event::IoEvent::Read;
    }
    if (write_callback_) {
        events |= event::IoEvent::Write;
    }
    if (terminal_callback_) {
        events |= event::IoEvent::Terminal;
    }
    return events;
}

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
    auto result = rwfd->begin_wait(waiter);
    if (!result || !*result) {
        waiter->err_ = result ? fiber::common::IoErr::None : result.error();
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
