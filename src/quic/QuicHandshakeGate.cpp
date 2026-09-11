#include <fiber/quic/QuicHandshakeGate.h>

#include <coroutine>

#include <fiber/async/WaitAwaiter.h>
#include <fiber/common/Assert.h>
#include <fiber/event/EventLoop.h>
#include <fiber/quic/QuicConnection.h>

namespace fiber::quic {

// One parked handshake wait. Its result is re-derived from connection state on
// every notification rather than pushed in, because two waiters on the same
// connection can want different things: reaching Established answers one
// waiting for it and leaves another waiting for confirmation parked.
class QuicHandshakeGate::Waiter : public async::WaitAwaiter {
public:
    Waiter(QuicHandshakeGate &gate, bool confirmed, std::chrono::steady_clock::time_point deadline) noexcept :
        WaitAwaiter(deadline, &Waiter::detach_from_gate, common::IoErr::WouldBlock), gate_(&gate),
        wait_confirmed_(confirmed) {}

    ~Waiter() { detach(); }

    bool await_ready() noexcept {
        set_result(gate_->wait_result(wait_confirmed_));
        if (result() != common::IoErr::WouldBlock) {
            mark_completed();
            return true;
        }
        if (timed_out(std::chrono::steady_clock::now())) {
            set_result(common::IoErr::TimedOut);
            mark_completed();
            return true;
        }
        return false;
    }

    bool await_suspend(std::coroutine_handle<> handle) noexcept {
        set_result(gate_->wait_result(wait_confirmed_));
        if (result() != common::IoErr::WouldBlock) {
            mark_completed();
            return false;
        }
        event::EventLoop *loop = event::EventLoop::current_or_null();
        FIBER_ASSERT(loop != nullptr);
        FIBER_ASSERT(&gate_->connection_->loop() == loop);
        if (timed_out(loop->now())) {
            set_result(common::IoErr::TimedOut);
            mark_completed();
            return false;
        }

        gate_->link(*this);
        begin_wait(handle, *loop);
        return true;
    }

    common::IoErr await_resume() noexcept {
        const common::IoErr outcome = result();
        end_wait();
        detach();
        // A waiter can only leave the queue completed, so WouldBlock here means
        // the wait was abandoned rather than answered.
        return outcome == common::IoErr::WouldBlock ? common::IoErr::Canceled : outcome;
    }

    [[nodiscard]] bool wait_confirmed() const noexcept { return wait_confirmed_; }
    [[nodiscard]] Waiter *next() const noexcept { return next_; }

private:
    static void detach_from_gate(async::WaitAwaiter &base) noexcept {
        auto &self = static_cast<Waiter &>(base);
        if (self.linked_) {
            self.gate_->unlink(self);
        }
    }

    QuicHandshakeGate *gate_ = nullptr;
    bool wait_confirmed_ = false;

public:
    Waiter *prev_ = nullptr;
    Waiter *next_ = nullptr;
    bool linked_ = false;
};

QuicHandshakeGate::~QuicHandshakeGate() { FIBER_ASSERT(head_ == nullptr && tail_ == nullptr); }

async::Task<common::IoResult<void>> QuicHandshakeGate::wait(bool confirmed,
                                                            std::chrono::milliseconds timeout) noexcept {
    if (timeout < std::chrono::milliseconds::zero()) {
        timeout = std::chrono::milliseconds::zero();
    }
    const std::chrono::steady_clock::time_point deadline = timeout == std::chrono::milliseconds::max()
                                                                   ? std::chrono::steady_clock::time_point::max()
                                                                   : event::EventLoop::current().now() + timeout;
    const common::IoErr result = co_await Waiter(*this, confirmed, deadline);
    if (result != common::IoErr::None) {
        co_return std::unexpected(result);
    }
    co_return common::IoResult<void>{};
}

common::IoErr QuicHandshakeGate::wait_result(bool confirmed) const noexcept {
    const QuicConnection &connection = *connection_;
    const QuicConnectionState state = connection.state();
    if (state == QuicConnectionState::Established && (!confirmed || connection.handshake_confirmed())) {
        return common::IoErr::None;
    }
    if (connection.connect_failure() != common::IoErr::None) {
        return connection.connect_failure();
    }
    if (state == QuicConnectionState::Init || state == QuicConnectionState::Handshaking ||
        (state == QuicConnectionState::Established && confirmed)) {
        return common::IoErr::WouldBlock;
    }
    switch (connection.close_source()) {
        case QuicCloseSource::IdleTimeout:
            return common::IoErr::TimedOut;
        case QuicCloseSource::PeerConnectionClose:
        case QuicCloseSource::StatelessReset:
            return common::IoErr::ConnReset;
        case QuicCloseSource::None:
        case QuicCloseSource::Local:
            return common::IoErr::Canceled;
    }
    return common::IoErr::Canceled;
}

void QuicHandshakeGate::notify(common::IoErr result) noexcept {
    // complete() unlinks, so the next waiter is read before completing this
    // one; a waiter that stays parked keeps its place in the walk.
    Waiter *waiter = head_;
    while (waiter != nullptr) {
        Waiter *next = waiter->next();
        const common::IoErr waiter_result =
                result == common::IoErr::WouldBlock ? wait_result(waiter->wait_confirmed()) : result;
        if (waiter_result != common::IoErr::WouldBlock) {
            waiter->complete(waiter_result);
        }
        waiter = next;
    }
}

void QuicHandshakeGate::link(Waiter &waiter) noexcept {
    FIBER_ASSERT(!waiter.linked_);
    waiter.prev_ = tail_;
    waiter.next_ = nullptr;
    if (tail_ != nullptr) {
        tail_->next_ = &waiter;
    } else {
        head_ = &waiter;
    }
    tail_ = &waiter;
    waiter.linked_ = true;
    ++count_;
}

void QuicHandshakeGate::unlink(Waiter &waiter) noexcept {
    FIBER_ASSERT(waiter.linked_);
    if (waiter.prev_ != nullptr) {
        waiter.prev_->next_ = waiter.next_;
    } else {
        head_ = waiter.next_;
    }
    if (waiter.next_ != nullptr) {
        waiter.next_->prev_ = waiter.prev_;
    } else {
        tail_ = waiter.prev_;
    }
    waiter.prev_ = nullptr;
    waiter.next_ = nullptr;
    waiter.linked_ = false;
    FIBER_ASSERT(count_ != 0);
    --count_;
}

} // namespace fiber::quic
