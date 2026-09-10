#include <fiber/quic/QuicLocalStreamGate.h>

#include <coroutine>
#include <utility>

#include <fiber/common/Assert.h>
#include <fiber/event/EventLoop.h>

namespace fiber::quic {

// One parked attach request. The waiter stays linked while it waits, including
// after it has been told that credit is available: that is what keeps
// try_attach yielding to it, and it is why the gate needs no reservation
// counter. A terminal result unlinks immediately so a closing connection leaves
// no waiter behind, even though the coroutine resumes later on the loop.
class QuicLocalStreamGate::Waiter {
public:
    Waiter(QuicLocalStreamGate &gate, QuicStreamType type, std::chrono::steady_clock::time_point deadline) noexcept :
        gate_(&gate), loop_(&event::EventLoop::current()), deadline_(deadline), type_(type) {
        gate_->link_waiter(*this);
        arm_timer();
    }

    Waiter(const Waiter &) = delete;
    Waiter &operator=(const Waiter &) = delete;
    Waiter(Waiter &&) = delete;
    Waiter &operator=(Waiter &&) = delete;

    ~Waiter() {
        cancel_timer();
        if (resume_posted_) {
            loop_->cancel<Waiter, &Waiter::notify_entry_>(*this);
            resume_posted_ = false;
        }
        if (linked_) {
            gate_->unlink_waiter(*this);
        }
    }

    [[nodiscard]] bool await_ready() const noexcept { return result_ != common::IoErr::None || signaled_; }

    bool await_suspend(std::coroutine_handle<> handle) noexcept {
        handle_ = handle;
        return true;
    }

    common::IoErr await_resume() noexcept {
        signaled_ = false;
        return result_;
    }

    // Credit is available; keep the waiter queued until it actually attaches.
    void signal_available() noexcept {
        if (signaled_ || result_ != common::IoErr::None) {
            return;
        }
        signaled_ = true;
        post_resume();
    }

    // Terminal outcome. The caller unlinks first.
    void complete(common::IoErr result) noexcept {
        FIBER_ASSERT(result != common::IoErr::None);
        if (result_ != common::IoErr::None) {
            return;
        }
        result_ = result;
        cancel_timer();
        post_resume();
    }

    [[nodiscard]] bool signaled() const noexcept { return signaled_; }
    [[nodiscard]] QuicStreamType type() const noexcept { return type_; }

private:
    [[nodiscard]] bool has_timer() const noexcept { return deadline_ != std::chrono::steady_clock::time_point::max(); }

    void arm_timer() noexcept {
        if (!has_timer()) {
            return;
        }
        loop_->post_at<Waiter, &Waiter::timer_entry_, &Waiter::on_timeout>(deadline_, *this);
    }

    void cancel_timer() noexcept {
        if (timer_entry_.is_in_heap()) {
            loop_->cancel<Waiter, &Waiter::timer_entry_>(*this);
        }
    }

    void post_resume() noexcept {
        if (resume_posted_) {
            return;
        }
        resume_posted_ = true;
        loop_->post_local<Waiter, &Waiter::notify_entry_, &Waiter::on_notify>(*this);
    }

    static void on_notify(Waiter *waiter) noexcept {
        FIBER_ASSERT(waiter != nullptr);
        waiter->resume_posted_ = false;
        std::coroutine_handle<> handle = std::exchange(waiter->handle_, {});
        if (handle) {
            handle.resume();
        }
    }

    static void on_timeout(Waiter *waiter) noexcept {
        FIBER_ASSERT(waiter != nullptr);
        if (waiter->result_ != common::IoErr::None) {
            return;
        }
        if (waiter->linked_) {
            waiter->gate_->unlink_waiter(*waiter);
        }
        waiter->complete(common::IoErr::TimedOut);
    }

    QuicLocalStreamGate *gate_ = nullptr;
    event::EventLoop *loop_ = nullptr;
    std::coroutine_handle<> handle_{};
    std::chrono::steady_clock::time_point deadline_{};
    event::EventLoop::DeferEntry notify_entry_{};
    event::EventLoop::TimerEntry timer_entry_{};
    common::IoErr result_ = common::IoErr::None;
    QuicStreamType type_ = QuicStreamType::Bidirectional;
    bool signaled_ = false;
    bool resume_posted_ = false;

public:
    Waiter *prev_ = nullptr;
    Waiter *next_ = nullptr;
    bool linked_ = false;
};

QuicLocalStreamGate::QuicLocalStreamGate(QuicConnection &connection) noexcept : connection_(&connection) {}

QuicLocalStreamGate::~QuicLocalStreamGate() { cancel_all(common::IoErr::Canceled); }

std::size_t QuicLocalStreamGate::waiter_count(QuicStreamType type) const noexcept { return queue_for(type).count; }

common::IoResult<QuicStream *> QuicLocalStreamGate::try_attach(QuicStream::Lease &&stream, QuicStreamType type,
                                                               QuicStreamEarlyDataMode early_data_mode) noexcept {
    if (queue_for(type).head != nullptr) {
        const common::IoErr status = connection_->local_stream_attach_status(type, early_data_mode);
        return std::unexpected(status == common::IoErr::None ? common::IoErr::Busy : status);
    }
    return connection_->try_attach_local_stream(std::move(stream), type, early_data_mode);
}

async::Task<common::IoResult<QuicStream *>>
QuicLocalStreamGate::attach(QuicStream::Lease stream, QuicStreamType type, std::chrono::milliseconds timeout,
                            QuicStreamEarlyDataMode early_data_mode) noexcept {
    auto immediate = try_attach(std::move(stream), type, early_data_mode);
    if (immediate || immediate.error() != common::IoErr::Busy) {
        co_return immediate;
    }
    // A replay-safe caller wants 0-RTT or an answer, never a wait for 1-RTT.
    if (early_data_mode == QuicStreamEarlyDataMode::ReplaySafe &&
        connection_->state() != QuicConnectionState::Established) {
        co_return std::unexpected(common::IoErr::Busy);
    }
    if (timeout <= std::chrono::milliseconds::zero()) {
        co_return std::unexpected(common::IoErr::TimedOut);
    }

    auto *loop = event::EventLoop::current_or_null();
    FIBER_ASSERT(loop != nullptr);
    FIBER_ASSERT(connection_->loop() == nullptr || connection_->loop() == loop);
    const std::chrono::steady_clock::time_point deadline = timeout == std::chrono::milliseconds::max()
                                                                   ? std::chrono::steady_clock::time_point::max()
                                                                   : loop->now() + timeout;

    Waiter waiter(*this, type, deadline);
    for (;;) {
        const common::IoErr wait_result = co_await waiter;
        if (wait_result != common::IoErr::None) {
            co_return std::unexpected(wait_result);
        }
        // The waiter is still queued, so this goes straight to the connection:
        // it is the request the freed credit was woken for.
        auto attached = connection_->try_attach_local_stream(std::move(stream), type, early_data_mode);
        if (attached || attached.error() != common::IoErr::Busy) {
            co_return attached;
        }
    }
}

void QuicLocalStreamGate::cancel_all(common::IoErr reason) noexcept {
    cancel_all(QuicStreamType::Bidirectional, reason);
    cancel_all(QuicStreamType::Unidirectional, reason);
}

void QuicLocalStreamGate::cancel_all(QuicStreamType type, common::IoErr reason) noexcept {
    FIBER_ASSERT(reason != common::IoErr::None);
    Queue &queue = queue_for(type);
    while (queue.head != nullptr) {
        Waiter *waiter = queue.head;
        unlink_waiter(*waiter);
        waiter->complete(reason);
    }
}

void QuicLocalStreamGate::on_capacity_change() noexcept {
    handle_change(QuicStreamType::Bidirectional);
    handle_change(QuicStreamType::Unidirectional);
}

void QuicLocalStreamGate::on_state_change() noexcept {
    handle_change(QuicStreamType::Bidirectional);
    handle_change(QuicStreamType::Unidirectional);
}

void QuicLocalStreamGate::handle_change(QuicStreamType type) noexcept {
    // Every parked waiter is OneRttOnly: a ReplaySafe caller is answered
    // immediately and never reaches the queue.
    const common::IoErr status = connection_->local_stream_attach_status(type);
    if (status == common::IoErr::None) {
        wake_waiters(type);
    } else if (status != common::IoErr::Busy) {
        cancel_all(type, status);
    }
}

void QuicLocalStreamGate::wake_waiters(QuicStreamType type) noexcept {
    std::uint64_t available = connection_->available_local_stream_slots(type);
    for (Waiter *waiter = queue_for(type).head; waiter != nullptr && available != 0; waiter = waiter->next_) {
        // An already woken waiter still owes an attach, so its credit is spoken
        // for: count it, but do not wake anyone twice for the same stream id.
        if (!waiter->signaled()) {
            waiter->signal_available();
        }
        --available;
    }
}

void QuicLocalStreamGate::link_waiter(Waiter &waiter) noexcept {
    FIBER_ASSERT(!waiter.linked_);
    Queue &queue = queue_for(waiter.type());
    waiter.prev_ = queue.tail;
    waiter.next_ = nullptr;
    if (queue.tail != nullptr) {
        queue.tail->next_ = &waiter;
    } else {
        queue.head = &waiter;
    }
    queue.tail = &waiter;
    waiter.linked_ = true;
    ++queue.count;
}

void QuicLocalStreamGate::unlink_waiter(Waiter &waiter) noexcept {
    FIBER_ASSERT(waiter.linked_);
    Queue &queue = queue_for(waiter.type());
    if (waiter.prev_ != nullptr) {
        waiter.prev_->next_ = waiter.next_;
    } else {
        queue.head = waiter.next_;
    }
    if (waiter.next_ != nullptr) {
        waiter.next_->prev_ = waiter.prev_;
    } else {
        queue.tail = waiter.prev_;
    }
    waiter.prev_ = nullptr;
    waiter.next_ = nullptr;
    waiter.linked_ = false;
    FIBER_ASSERT(queue.count != 0);
    --queue.count;
}

} // namespace fiber::quic
