#include <fiber/net/detail/RWFd.h>

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
    if (err == fiber::common::IoErr::None) {
        // Normal transition: drop this subscription before completing; the
        // direction's readiness and kernel interest are untouched.
        (void) waiter->rwfd_->cancel_wait(waiter);
    }
    // Canceled completions arrive from close(), which has already detached the
    // waiter; the RWFd may be destroyed by then and must not be touched.
    FIBER_ASSERT(waiter->complete_callback_ != nullptr);
    waiter->complete_callback_(waiter, err);
}

RWFd::RWFd(event::EventLoop &owner_loop, Kind kind) :
    kind_(kind), efd_(owner_loop, this, &RWFd::on_efd_events, event::Poller::Mode::Edge) {}

RWFd::RWFd(event::EventLoop &owner_loop, int socket, Kind kind) : RWFd(owner_loop, kind) {
    const auto err = attach(socket);
    FIBER_ASSERT(err == common::IoErr::None);
}

RWFd::~RWFd() {
    if (dispatch_destroyed_observer_) {
        *dispatch_destroyed_observer_ = true;
        dispatch_destroyed_observer_ = nullptr;
    }
    if (efd_.current_loop().in_loop()) {
        close();
    } else {
        // Off the current loop only a fully detached object may die: the
        // handover protocol guarantees there is no registration or subscription
        // left for any loop to dispatch through.
        FIBER_ASSERT(!efd_.registered() && !has_callbacks());
    }
}

bool RWFd::valid() const noexcept { return efd_.valid(); }
int RWFd::fd() const noexcept { return efd_.fd(); }
event::EventLoop &RWFd::owner_loop() const noexcept { return efd_.owner_loop(); }
event::EventLoop &RWFd::current_loop() const noexcept { return efd_.current_loop(); }
event::EventLoop &RWFd::loop() const noexcept { return efd_.current_loop(); }

common::IoErr RWFd::attach(int socket) noexcept {
    const auto err = efd_.attach(socket);
    if (err == common::IoErr::None) {
        read_event_.state = State::Unknown;
        write_event_.state = State::Unknown;
    }
    return err;
}

int RWFd::release_fd() noexcept {
    // An fd with no poller registration belongs to no loop: the constructing
    // thread may release it during a startup rollback.
    FIBER_ASSERT(!efd_.registered() || efd_.current_loop().in_loop());
    FIBER_ASSERT(!has_callbacks());
    if (efd_.registered() && efd_.unwatch_all() != common::IoErr::None) {
        return -1;
    }
    read_event_.state = State::Unknown;
    write_event_.state = State::Unknown;
    return efd_.release_fd();
}

void RWFd::set_stream_event_sink(void *ctx, StreamEventCallback callback) noexcept {
    FIBER_ASSERT(callback != nullptr);
    stream_sink_ctx_ = ctx;
    stream_sink_ = callback;
}

common::IoErr RWFd::ensure_state_observation() noexcept {
    if (kind_ != Kind::Stream) {
        return common::IoErr::None;
    }
    return ensure_listening(event::IoEvent::ReadHangup);
}

RWFd::DetachedCompletions RWFd::detach_for_close() noexcept {
    FIBER_ASSERT(efd_.current_loop().in_loop());
    DetachedCompletions detached;
    detached.read_callback = std::exchange(read_event_.callback, nullptr);
    detached.read_ctx = std::exchange(read_event_.ctx, nullptr);
    detached.write_callback = std::exchange(write_event_.callback, nullptr);
    detached.write_ctx = std::exchange(write_event_.ctx, nullptr);
    if (read_generation_ != UINT64_MAX) {
        ++read_generation_;
    }
    if (write_generation_ != UINT64_MAX) {
        ++write_generation_;
    }
    efd_.close_fd();
    return detached;
}

void RWFd::DetachedCompletions::complete(common::IoErr err) const noexcept {
    auto complete_one = [err](ReadyCallback callback, void *ctx) noexcept {
        if (callback == &RWFdWaiterBase::on_event) {
            // The waiter has already been detached. In particular, its RWFd may
            // be gone by the time the previous completion ran.
            auto *waiter = static_cast<RWFdWaiterBase *>(ctx);
            waiter->complete_callback_(waiter, err);
        } else if (callback) {
            callback(ctx, err);
        }
    };
    complete_one(read_callback, read_ctx);
    complete_one(write_callback, write_ctx);
}

void RWFd::close() { detach_for_close().complete(common::IoErr::Canceled); }

common::IoErr RWFd::ensure_listening(event::IoEvent events) noexcept {
    FIBER_ASSERT(efd_.current_loop().in_loop());
    if (!valid()) {
        return common::IoErr::BadFd;
    }
    return efd_.watch_add(events);
}

common::IoErr RWFd::install_callback(Event &event, std::uint64_t &generation, event::IoEvent direction,
                                     ReadyCallback callback, void *ctx) noexcept {
    FIBER_ASSERT(efd_.current_loop().in_loop());
    if (!callback) {
        return common::IoErr::Invalid;
    }
    // A Ready caller must advance by doing I/O, not by subscribing for another
    // edge, and only one subscription may exist per direction at a time.
    FIBER_ASSERT(event.state != State::Ready);
    if (event.callback) {
        return common::IoErr::Busy;
    }
    if (generation == UINT64_MAX) {
        return common::IoErr::Invalid;
    }
    const auto err = ensure_listening(direction);
    if (err != common::IoErr::None) {
        return err;
    }
    event.callback = callback;
    event.ctx = ctx;
    ++generation;
    return common::IoErr::None;
}

common::IoErr RWFd::set_read_callback(ReadyCallback callback, void *ctx) noexcept {
    return install_callback(read_event_, read_generation_, event::IoEvent::Read, callback, ctx);
}

common::IoErr RWFd::set_write_callback(ReadyCallback callback, void *ctx) noexcept {
    return install_callback(write_event_, write_generation_, event::IoEvent::Write, callback, ctx);
}

bool RWFd::remove_callback(Event &event, std::uint64_t &generation, event::IoEvent direction, ReadyCallback callback,
                           void *ctx) noexcept {
    (void) direction;
    if (event.callback != callback || event.ctx != ctx) {
        return false;
    }
    event.callback = nullptr;
    event.ctx = nullptr;
    // Saturation rejects later subscriptions instead of wrapping their identity.
    if (generation != UINT64_MAX) {
        ++generation;
    }
    return true;
}

common::IoErr RWFd::clear_read_callback(ReadyCallback callback, void *ctx) noexcept {
    FIBER_ASSERT(efd_.current_loop().in_loop());
    if (!callback) {
        return common::IoErr::Invalid;
    }
    (void) remove_callback(read_event_, read_generation_, event::IoEvent::Read, callback, ctx);
    return common::IoErr::None;
}

common::IoErr RWFd::clear_write_callback(ReadyCallback callback, void *ctx) noexcept {
    FIBER_ASSERT(efd_.current_loop().in_loop());
    if (!callback) {
        return common::IoErr::Invalid;
    }
    (void) remove_callback(write_event_, write_generation_, event::IoEvent::Write, callback, ctx);
    return common::IoErr::None;
}

common::IoErr RWFd::detach_for_handover() noexcept {
    FIBER_ASSERT(efd_.current_loop().in_loop());
    FIBER_ASSERT(!has_callbacks());
    return efd_.detach_from_current_loop();
}

common::IoErr RWFd::adopt_loop(event::EventLoop &loop) noexcept {
    efd_.adopt_loop(loop);
    read_event_.state = State::Unknown;
    write_event_.state = State::Unknown;
    // The fd is closed by its owner before the loop stops; adoption onto a
    // loop that is already stopping is refused up front, because nobody would
    // run a later cleanup there.
    if (loop.stopping()) {
        close();
        return common::IoErr::Canceled;
    }
    return common::IoErr::None;
}

RWFd::WaitReadableAwaiter RWFd::wait_readable(std::chrono::milliseconds timeout, StreamWaitGate gate) noexcept {
    return WaitReadableAwaiter(*this, timeout, gate);
}

RWFd::WaitWritableAwaiter RWFd::wait_writable(std::chrono::milliseconds timeout, StreamWaitGate gate) noexcept {
    return WaitWritableAwaiter(*this, timeout, gate);
}

common::IoErr RWFd::begin_wait(RWFdWaiterBase *waiter) noexcept {
    if (waiter->event_ == event::IoEvent::Read) {
        return install_callback(read_event_, read_generation_, event::IoEvent::Read, &RWFdWaiterBase::on_event, waiter);
    }
    return install_callback(write_event_, write_generation_, event::IoEvent::Write, &RWFdWaiterBase::on_event, waiter);
}

void RWFd::cancel_wait(RWFdWaiterBase *waiter) noexcept {
    FIBER_ASSERT(efd_.current_loop().in_loop());
    (void) remove_callback(waiter->event_ == event::IoEvent::Read ? read_event_ : write_event_,
                           waiter->event_ == event::IoEvent::Read ? read_generation_ : write_generation_,
                           waiter->event_, &RWFdWaiterBase::on_event, waiter);
}

void RWFd::on_efd_events(void *owner, event::IoEvent events) { static_cast<RWFd *>(owner)->handle_events(events); }

void RWFd::handle_events(event::IoEvent events) {
    FIBER_ASSERT(efd_.current_loop().in_loop());
    if (!valid()) {
        return;
    }

    DispatchGuard guard(*this);
    const auto epoch = efd_.epoch();

    // Raw stream bits this layer does not interpret: forwarded to the adapter
    // sink first so observable stream state settles before business callbacks.
    const event::IoEvent stream_bits = events & (event::IoEvent::ReadHangup | event::IoEvent::Terminal);
    // A read-side hangup still delivers buffered data and then EOF; a terminal
    // event fails both directions' pending operations.
    if (event::any(events & event::IoEvent::ReadHangup)) {
        events |= event::IoEvent::Read;
    }
    if (event::any(events & event::IoEvent::Terminal)) {
        events |= event::IoEvent::Read | event::IoEvent::Write;
    }

    // Record every transition first. Callbacks may immediately run I/O and
    // re-mark their direction; a later step must not clobber that result.
    const bool read_transition = event::any(events & event::IoEvent::Read) && read_event_.state != State::Ready;
    const bool write_transition = event::any(events & event::IoEvent::Write) && write_event_.state != State::Ready;
    if (event::any(events & event::IoEvent::Read)) {
        read_event_.state = State::Ready;
    }
    if (event::any(events & event::IoEvent::Write)) {
        write_event_.state = State::Ready;
    }
    const auto read_generation = read_generation_;
    const auto write_generation = write_generation_;

    if (stream_sink_ != nullptr && event::any(stream_bits)) {
        stream_sink_(stream_sink_ctx_, stream_bits);
        if (guard.owner_destroyed() || !valid() || efd_.epoch() != epoch) {
            return;
        }
    }

    // Ready -> Ready never re-notifies; a subscription installed while this
    // dispatch was running has a newer generation and is not notified either.
    if (read_transition && read_event_.callback != nullptr && read_generation_ == read_generation) {
        read_event_.callback(read_event_.ctx, common::IoErr::None);
        if (guard.owner_destroyed() || !valid() || efd_.epoch() != epoch) {
            return;
        }
    }
    if (write_transition && write_event_.callback != nullptr && write_generation_ == write_generation) {
        write_event_.callback(write_event_.ctx, common::IoErr::None);
    }
}

bool RWFd::has_callbacks() const noexcept {
    return read_event_.callback != nullptr || write_event_.callback != nullptr;
}

template class RWFd::WaitAwaiter<fiber::event::IoEvent::Read>;
template class RWFd::WaitAwaiter<fiber::event::IoEvent::Write>;

} // namespace fiber::net::detail
