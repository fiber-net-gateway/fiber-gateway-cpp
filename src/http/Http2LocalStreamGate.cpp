#include <fiber/http/Http2LocalStreamGate.h>

#include <coroutine>
#include <utility>

#include <fiber/common/Assert.h>
#include <fiber/event/EventLoop.h>

namespace fiber::http {

// One parked attach request. The waiter stays linked while it waits, including
// after it has been told that capacity is available: that is what keeps
// try_attach yielding to it, and it is why the gate needs no reservation
// counter. A terminal result unlinks immediately so a closing connection leaves
// no waiter behind, even though the coroutine resumes later on the loop.
class Http2LocalStreamGate::Waiter {
public:
    Waiter(Http2LocalStreamGate &gate, std::chrono::steady_clock::time_point deadline) noexcept :
        gate_(&gate), loop_(&event::EventLoop::current()), deadline_(deadline) {
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

    // Capacity is available; keep the waiter queued until it actually attaches.
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

    Http2LocalStreamGate *gate_ = nullptr;
    event::EventLoop *loop_ = nullptr;
    std::coroutine_handle<> handle_{};
    std::chrono::steady_clock::time_point deadline_{};
    event::EventLoop::DeferEntry notify_entry_{};
    event::EventLoop::TimerEntry timer_entry_{};
    common::IoErr result_ = common::IoErr::None;
    bool signaled_ = false;
    bool resume_posted_ = false;

public:
    Waiter *prev_ = nullptr;
    Waiter *next_ = nullptr;
    bool linked_ = false;
};

Http2LocalStreamGate::Http2LocalStreamGate(Http2Connection &connection) noexcept : connection_(&connection) {
    connection_->set_capacity_callback(&Http2LocalStreamGate::on_connection_capacity, this);
}

Http2LocalStreamGate::~Http2LocalStreamGate() {
    cancel_all(common::IoErr::Canceled);
    connection_->clear_capacity_callback();
}

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

void Http2LocalStreamGate::on_connection_capacity(void *ctx, Http2Connection &) noexcept {
    auto *gate = static_cast<Http2LocalStreamGate *>(ctx);
    FIBER_ASSERT(gate != nullptr);
    gate->handle_capacity_change();
}

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
    FIBER_ASSERT(!waiter.linked_);
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
