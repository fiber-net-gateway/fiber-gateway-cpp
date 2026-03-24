#include "Http2ControlFrameQueue.h"

#include <new>

#include "../common/Assert.h"
#include "HttpTransport.h"

namespace fiber::http {

struct Http2ControlFrameQueue::Slab {
    explicit Slab(std::size_t cap) noexcept : capacity(cap) {}

    [[nodiscard]] std::uint8_t *data() noexcept {
        return reinterpret_cast<std::uint8_t *>(this + 1);
    }

    [[nodiscard]] const std::uint8_t *data() const noexcept {
        return reinterpret_cast<const std::uint8_t *>(this + 1);
    }

    [[nodiscard]] std::size_t writable_bytes() const noexcept {
        return capacity - commit_pos;
    }

    static Slab *allocate(std::size_t capacity) noexcept {
        void *mem = ::operator new(sizeof(Slab) + capacity, std::nothrow);
        if (!mem) {
            return nullptr;
        }
        return new (mem) Slab(capacity);
    }

    static void destroy(Slab *slab) noexcept {
        if (!slab) {
            return;
        }
        slab->~Slab();
        ::operator delete(slab);
    }

    Slab *next = nullptr;
    std::size_t read_pos = 0;
    std::size_t commit_pos = 0;
    std::size_t capacity = 0;
};

Http2ControlFrameQueue::Http2ControlFrameQueue(HttpTransport *transport, std::size_t slab_capacity,
                                               std::chrono::milliseconds write_timeout) noexcept
    : transport_(transport),
      write_timeout_(write_timeout),
      slab_capacity_(slab_capacity) {
}

Http2ControlFrameQueue::~Http2ControlFrameQueue() {
    FIBER_ASSERT(!send_loop_running_);

    while (head_slab_) {
        Slab *next = head_slab_->next;
        Slab::destroy(head_slab_);
        head_slab_ = next;
    }
    tail_slab_ = nullptr;

    if (cached_empty_slab_) {
        Slab::destroy(cached_empty_slab_);
        cached_empty_slab_ = nullptr;
    }
}

Http2ControlFrameQueue::WaitForDataAwaiter::~WaitForDataAwaiter() {
    if (!queue_) {
        return;
    }
    queue_->cancel_waiter(this);
}

bool Http2ControlFrameQueue::WaitForDataAwaiter::await_ready() const noexcept {
    return queue_ == nullptr || queue_->should_wake_waiter();
}

bool Http2ControlFrameQueue::WaitForDataAwaiter::await_suspend(std::coroutine_handle<> handle) noexcept {
    if (!queue_) {
        return false;
    }

    loop_ = fiber::event::EventLoop::current_or_null();
    FIBER_ASSERT(loop_ != nullptr);
    handle_ = handle;
    return queue_->arm_waiter(this);
}

void Http2ControlFrameQueue::WaitForDataAwaiter::await_resume() noexcept {
    if (!queue_) {
        return;
    }
    if (queue_->waiter_ == this) {
        queue_->waiter_ = nullptr;
    }
    queue_ = nullptr;
    loop_ = nullptr;
    handle_ = {};
    resume_posted_ = false;
}

void Http2ControlFrameQueue::WaitForDataAwaiter::on_notify(WaitForDataAwaiter *awaiter) {
    if (!awaiter) {
        return;
    }
    awaiter->resume_posted_ = false;
    awaiter->resume();
}

void Http2ControlFrameQueue::WaitForDataAwaiter::resume() noexcept {
    auto handle = handle_;
    handle_ = {};
    if (handle) {
        handle.resume();
    }
}

std::size_t Http2ControlFrameQueue::active_slab_count() const noexcept {
    std::size_t count = 0;
    for (Slab *slab = head_slab_; slab; slab = slab->next) {
        ++count;
    }
    return count;
}

void Http2ControlFrameQueue::bind_owner_loop_if_needed() noexcept {
    fiber::event::EventLoop *current = fiber::event::EventLoop::current_or_null();
    if (!current) {
        return;
    }
    if (!owner_loop_) {
        owner_loop_ = current;
        return;
    }
    FIBER_ASSERT(owner_loop_ == current);
}

common::IoErr Http2ControlFrameQueue::reserve_tail(std::size_t bytes, Reservation &reservation) noexcept {
    if (bytes == 0 || bytes > slab_capacity_) {
        return common::IoErr::Invalid;
    }
    if (stop_reason_ != common::IoErr::None) {
        return stop_reason_;
    }
    if (closed_) {
        return common::IoErr::Canceled;
    }

    Slab *slab = tail_slab_;
    if (!slab || slab->writable_bytes() < bytes) {
        slab = acquire_slab();
        if (!slab) {
            return common::IoErr::NoMem;
        }
        append_tail_slab(slab);
    }

    reservation.slab = slab;
    reservation.data = slab->data() + slab->commit_pos;
    reservation.begin = slab->commit_pos;
    reservation.bytes = bytes;
    return common::IoErr::None;
}

void Http2ControlFrameQueue::rollback_reservation(const Reservation &) noexcept {
}

void Http2ControlFrameQueue::commit_reservation(const Reservation &reservation) noexcept {
    FIBER_ASSERT(reservation.slab != nullptr);
    FIBER_ASSERT(reservation.begin == reservation.slab->commit_pos);
    reservation.slab->commit_pos += reservation.bytes;
    pending_bytes_ += reservation.bytes;
}

bool Http2ControlFrameQueue::arm_waiter(WaitForDataAwaiter *awaiter) noexcept {
    if (!awaiter || should_wake_waiter()) {
        return false;
    }

    FIBER_ASSERT(waiter_ == nullptr);
    waiter_ = awaiter;
    return true;
}

void Http2ControlFrameQueue::cancel_waiter(WaitForDataAwaiter *awaiter) noexcept {
    if (waiter_ == awaiter) {
        waiter_ = nullptr;
    }
}

void Http2ControlFrameQueue::notify_waiter() noexcept {
    if (!waiter_ || waiter_->resume_posted_ || waiter_->loop_ == nullptr) {
        return;
    }

    waiter_->resume_posted_ = true;
    waiter_->loop_->post<WaitForDataAwaiter, &WaitForDataAwaiter::notify_entry_, &WaitForDataAwaiter::on_notify>(*waiter_);
}

bool Http2ControlFrameQueue::should_wake_waiter() const noexcept {
    return pending_bytes_ != 0 || closed_ || aborting_ || stop_reason_ != common::IoErr::None;
}

Http2ControlFrameQueue::Slab *Http2ControlFrameQueue::acquire_slab() noexcept {
    if (cached_empty_slab_) {
        Slab *slab = cached_empty_slab_;
        cached_empty_slab_ = nullptr;
        slab->next = nullptr;
        slab->read_pos = 0;
        slab->commit_pos = 0;
        return slab;
    }

    return Slab::allocate(slab_capacity_);
}

void Http2ControlFrameQueue::recycle_slab(Slab *slab) noexcept {
    if (!slab) {
        return;
    }

    slab->next = nullptr;
    slab->read_pos = 0;
    slab->commit_pos = 0;
    if (!cached_empty_slab_) {
        cached_empty_slab_ = slab;
        return;
    }

    Slab::destroy(slab);
}

void Http2ControlFrameQueue::append_tail_slab(Slab *slab) noexcept {
    FIBER_ASSERT(slab != nullptr);
    slab->next = nullptr;
    if (tail_slab_) {
        tail_slab_->next = slab;
    } else {
        head_slab_ = slab;
    }
    tail_slab_ = slab;
}

void Http2ControlFrameQueue::discard_empty_head_slabs() noexcept {
    while (head_slab_ && head_slab_->read_pos == head_slab_->commit_pos) {
        Slab *next = head_slab_->next;
        recycle_slab(head_slab_);
        head_slab_ = next;
        if (!head_slab_) {
            tail_slab_ = nullptr;
        }
        sending_end_ = 0;
    }
}

Http2ControlFrameQueue::SendSpan Http2ControlFrameQueue::current_send_span() noexcept {
    discard_empty_head_slabs();
    FIBER_ASSERT(head_slab_ != nullptr);
    if (sending_end_ <= head_slab_->read_pos) {
        sending_end_ = head_slab_->commit_pos;
    }
    FIBER_ASSERT(sending_end_ > head_slab_->read_pos);
    return {head_slab_->data() + head_slab_->read_pos, sending_end_ - head_slab_->read_pos};
}

void Http2ControlFrameQueue::consume_written_bytes(std::size_t bytes) noexcept {
    FIBER_ASSERT(head_slab_ != nullptr);
    FIBER_ASSERT(bytes <= pending_bytes_);
    FIBER_ASSERT(head_slab_->read_pos + bytes <= sending_end_);

    head_slab_->read_pos += bytes;
    pending_bytes_ -= bytes;
    if (head_slab_->read_pos == sending_end_) {
        sending_end_ = head_slab_->read_pos;
    }
    discard_empty_head_slabs();
}

void Http2ControlFrameQueue::fail(common::IoErr reason) noexcept {
    if (stop_reason_ == common::IoErr::None) {
        stop_reason_ = reason;
    }
    aborting_ = true;
    closed_ = true;
    notify_waiter();
}

fiber::async::Task<void> Http2ControlFrameQueue::send_loop() noexcept {
    fiber::event::EventLoop *loop = fiber::event::EventLoop::current_or_null();
    FIBER_ASSERT(loop != nullptr);
    if (!owner_loop_) {
        owner_loop_ = loop;
    } else {
        FIBER_ASSERT(owner_loop_ == loop);
    }
    FIBER_ASSERT(!send_loop_running_);

    send_loop_running_ = true;
    for (;;) {
        discard_empty_head_slabs();

        if (aborting_) {
            break;
        }
        if (pending_bytes_ == 0) {
            if (closed_) {
                break;
            }
            co_await wait_for_data();
            continue;
        }

        if (!transport_ || !transport_->valid()) {
            fail(common::IoErr::Invalid);
            break;
        }

        SendSpan span = current_send_span();
        common::IoResult<size_t> result = co_await transport_->write(span.data, span.length, write_timeout_);
        if (!result) {
            fail(result.error());
            break;
        }
        if (*result == 0) {
            fail(common::IoErr::ConnReset);
            break;
        }

        consume_written_bytes(*result);
    }

    send_loop_running_ = false;
    co_return;
}

void Http2ControlFrameQueue::close() noexcept {
    closed_ = true;
    notify_waiter();
}

void Http2ControlFrameQueue::abort(common::IoErr reason) noexcept {
    fail(reason);
}

} // namespace fiber::http
