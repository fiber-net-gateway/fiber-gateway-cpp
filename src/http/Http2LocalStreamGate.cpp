#include <fiber/http/Http2LocalStreamGate.h>

#include <coroutine>
#include <utility>

#include <fiber/common/Assert.h>
#include <fiber/event/EventLoop.h>

namespace fiber::http {

// One parked attach request. The waiter stays linked while it waits, including
// after it has been told that capacity is available: that is what keeps
// try_attach yielding to it, and it is why the gate needs no reservation
// counter. A terminal result unlinks immediately -- WaitAwaiter::complete
// detaches -- so a closing connection leaves no waiter behind, even though the
// coroutine resumes later on the loop.
class Http2LocalStreamGate::Waiter : public fiber::async::WaitAwaiter {
public:
    Waiter(Http2LocalStreamGate &gate, std::chrono::steady_clock::time_point deadline) noexcept :
        WaitAwaiter(deadline, &Waiter::detach_from_gate), gate_(&gate) {
        gate_->link_waiter(*this);
        begin_wait({}, event::EventLoop::current());
    }

    ~Waiter() { detach(); }

    [[nodiscard]] bool await_ready() const noexcept { return result() != common::IoErr::None || signaled_; }

    bool await_suspend(std::coroutine_handle<> handle) noexcept {
        begin_wait(handle, *loop());
        return true;
    }

    common::IoErr await_resume() noexcept { return result(); }

    void retry_blocked() noexcept { signaled_ = false; }

    // Capacity is available; keep the waiter queued until it actually attaches.
    void signal_available() noexcept {
        if (signaled_ || result() != common::IoErr::None) {
            return;
        }
        signaled_ = true;
        post_resume();
    }

    [[nodiscard]] bool signaled() const noexcept { return signaled_; }

private:
    static void detach_from_gate(fiber::async::WaitAwaiter &base) noexcept {
        auto &self = static_cast<Waiter &>(base);
        if (self.linked_) {
            self.gate_->detach_waiter(self);
        }
    }

    Http2LocalStreamGate *gate_ = nullptr;
    bool signaled_ = false;

public:
    Waiter *prev_ = nullptr;
    Waiter *next_ = nullptr;
    bool linked_ = false;
};

Http2LocalStreamGate::Http2LocalStreamGate(Http2Connection &connection) noexcept : connection_(&connection) {}

Http2LocalStreamGate::~Http2LocalStreamGate() { cancel_all(common::IoErr::Canceled); }

void Http2LocalStreamGate::set_capacity_callback(Http2Connection::CapacityCallback cb, void *ctx) noexcept {
    capacity_cb_ = cb;
    capacity_ctx_ = cb != nullptr ? ctx : nullptr;
}

void Http2LocalStreamGate::clear_capacity_callback() noexcept {
    capacity_cb_ = nullptr;
    capacity_ctx_ = nullptr;
}

common::IoResult<Http2Stream::Lease> Http2LocalStreamGate::try_attach(Http2Stream &stream) noexcept {
    if (waiter_head_ != nullptr) {
        const common::IoErr status = connection_->local_stream_attach_status();
        return std::unexpected(status == common::IoErr::None ? common::IoErr::Busy : status);
    }
    return connection_->try_attach_local_stream(stream);
}

fiber::async::Task<common::IoResult<Http2Stream::Lease>>
Http2LocalStreamGate::attach(Http2Stream &stream, std::chrono::milliseconds timeout) noexcept {
    auto immediate = try_attach(stream);
    if (immediate || immediate.error() != common::IoErr::Busy) {
        co_return immediate;
    }
    if (timeout <= std::chrono::milliseconds::zero()) {
        co_return std::unexpected(common::IoErr::TimedOut);
    }

    auto *loop = event::EventLoop::current_or_null();
    FIBER_ASSERT(loop != nullptr);
    FIBER_ASSERT(&connection_->loop() == loop);
    const std::chrono::steady_clock::time_point deadline = timeout == std::chrono::milliseconds::max()
                                                                   ? std::chrono::steady_clock::time_point::max()
                                                                   : loop->now() + timeout;

    Waiter waiter(*this, deadline);
    for (;;) {
        const common::IoErr wait_result = co_await waiter;
        if (wait_result != common::IoErr::None) {
            co_return std::unexpected(wait_result);
        }
        // The waiter is still queued, so this goes straight to the connection:
        // it is the request the freed slot was woken for.
        auto attached = connection_->try_attach_local_stream(stream);
        if (attached || attached.error() != common::IoErr::Busy) {
            co_return attached;
        }
        waiter.retry_blocked();
    }
}

void Http2LocalStreamGate::cancel_all(common::IoErr reason) noexcept {
    FIBER_ASSERT(reason != common::IoErr::None);
    while (waiter_head_ != nullptr) {
        Waiter *waiter = waiter_head_;
        unlink_waiter(*waiter);
        waiter->complete(reason);
    }
}

void Http2LocalStreamGate::on_capacity_change() noexcept { handle_capacity_change(); }
void Http2LocalStreamGate::on_state_change() noexcept { handle_capacity_change(); }

void Http2LocalStreamGate::handle_capacity_change() noexcept {
    const common::IoErr status = connection_->local_stream_attach_status();
    if (status == common::IoErr::None) {
        wake_waiters();
    } else if (status != common::IoErr::Busy) {
        cancel_all(status);
    }
    if (capacity_cb_ != nullptr) {
        capacity_cb_(capacity_ctx_, *connection_);
    }
}

void Http2LocalStreamGate::wake_waiters() noexcept {
    std::size_t available = connection_->available_local_stream_slots();
    for (Waiter *waiter = waiter_head_; waiter != nullptr && available != 0; waiter = waiter->next_) {
        // An already woken waiter still owes an attach, so its slot is spoken
        // for: count it, but do not wake anyone twice for the same slot.
        if (!waiter->signaled()) {
            waiter->signal_available();
        }
        --available;
    }
}

void Http2LocalStreamGate::link_waiter(Waiter &waiter) noexcept {
    FIBER_ASSERT(!waiter.linked_ && !waiter.completed());
    waiter.prev_ = waiter_tail_;
    waiter.next_ = nullptr;
    if (waiter_tail_ != nullptr) {
        waiter_tail_->next_ = &waiter;
    } else {
        waiter_head_ = &waiter;
    }
    waiter_tail_ = &waiter;
    waiter.linked_ = true;
    ++waiter_count_;
}

void Http2LocalStreamGate::detach_waiter(Waiter &waiter) noexcept {
    unlink_waiter(waiter);
    // Only individual departures redistribute capacity. Bulk cancellation
    // pre-unlinks so that completion does not wake the rest of the queue.
    if (waiter_head_ != nullptr && connection_->local_stream_attach_status() == common::IoErr::None) {
        wake_waiters();
    }
}

void Http2LocalStreamGate::unlink_waiter(Waiter &waiter) noexcept {
    FIBER_ASSERT(waiter.linked_);
    if (waiter.prev_ != nullptr) {
        waiter.prev_->next_ = waiter.next_;
    } else {
        waiter_head_ = waiter.next_;
    }
    if (waiter.next_ != nullptr) {
        waiter.next_->prev_ = waiter.prev_;
    } else {
        waiter_tail_ = waiter.prev_;
    }
    waiter.prev_ = nullptr;
    waiter.next_ = nullptr;
    waiter.linked_ = false;
    FIBER_ASSERT(waiter_count_ != 0);
    --waiter_count_;
}

} // namespace fiber::http
