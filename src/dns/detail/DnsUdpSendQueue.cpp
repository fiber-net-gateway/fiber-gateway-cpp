#include <fiber/dns/detail/DnsUdpSendQueue.h>

#include <utility>

#include <fiber/common/Assert.h>

namespace fiber::dns::detail {

DnsUdpSendQueue::~DnsUdpSendQueue() {
    FIBER_ASSERT(head_ == nullptr);
    FIBER_ASSERT(tail_ == nullptr);
    FIBER_ASSERT(state_ == State::Idle);
    FIBER_ASSERT(!handoff_entry_.is_in_queue());
}

void DnsUdpSendQueue::init(event::EventLoop &loop) noexcept {
    FIBER_ASSERT(loop_ == nullptr);
    FIBER_ASSERT(head_ == nullptr);
    FIBER_ASSERT(tail_ == nullptr);
    FIBER_ASSERT(state_ == State::Idle);
    loop_ = &loop;
    closed_ = false;
}

void DnsUdpSendQueue::close(common::IoErr reason) noexcept {
    if (!loop_) {
        return;
    }
    FIBER_ASSERT(loop_->in_loop());
    if (closed_) {
        return;
    }
    FIBER_ASSERT(reason != common::IoErr::None);
    closed_ = true;

    if (handoff_entry_.is_in_queue()) {
        loop_->cancel<DnsUdpSendQueue, &DnsUdpSendQueue::handoff_entry_>(*this);
    }
    if (state_ == State::Handoff) {
        state_ = State::Idle;
    }

    AcquireAwaiter *waiter = head_;
    head_ = nullptr;
    tail_ = nullptr;
    while (waiter) {
        AcquireAwaiter *next = waiter->next_;
        waiter->prev_ = nullptr;
        waiter->next_ = nullptr;
        waiter->queued_ = false;
        waiter->granted_ = false;
        waiter->err_ = reason;
        auto handle = waiter->handle_;
        waiter->handle_ = {};
        if (handle) {
            handle.resume();
        }
        waiter = next;
    }
}

void DnsUdpSendQueue::reset() noexcept {
    FIBER_ASSERT(head_ == nullptr);
    FIBER_ASSERT(tail_ == nullptr);
    FIBER_ASSERT(state_ == State::Idle);
    if (loop_ && handoff_entry_.is_in_queue()) {
        FIBER_ASSERT(loop_->in_loop());
        loop_->cancel<DnsUdpSendQueue, &DnsUdpSendQueue::handoff_entry_>(*this);
    }
    loop_ = nullptr;
    closed_ = false;
}

bool DnsUdpSendQueue::fast_path_available() const noexcept {
    return loop_ != nullptr && !closed_ && state_ == State::Idle && head_ == nullptr;
}

bool DnsUdpSendQueue::idle() const noexcept {
    return state_ == State::Idle && head_ == nullptr && tail_ == nullptr && !handoff_entry_.is_in_queue();
}

DnsUdpSendQueue::Owner DnsUdpSendQueue::take_ownership_after_would_block() noexcept {
    FIBER_ASSERT(loop_ != nullptr);
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(fast_path_available());
    state_ = State::Owned;
    return Owner(*this);
}

DnsUdpSendQueue::AcquireAwaiter DnsUdpSendQueue::acquire() noexcept { return AcquireAwaiter(*this); }

DnsUdpSendQueue::Owner::Owner(Owner &&other) noexcept : queue_(std::exchange(other.queue_, nullptr)) {}

DnsUdpSendQueue::Owner &DnsUdpSendQueue::Owner::operator=(Owner &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    release();
    queue_ = std::exchange(other.queue_, nullptr);
    return *this;
}

DnsUdpSendQueue::Owner::~Owner() { release(); }

void DnsUdpSendQueue::Owner::release() noexcept {
    DnsUdpSendQueue *queue = std::exchange(queue_, nullptr);
    if (queue) {
        queue->finish_owner();
    }
}

DnsUdpSendQueue::AcquireAwaiter::~AcquireAwaiter() {
    if (queue_ && queued_) {
        queue_->unlink(*this);
    }
}

bool DnsUdpSendQueue::AcquireAwaiter::await_ready() noexcept {
    if (!queue_ || queue_->closed_) {
        err_ = common::IoErr::Canceled;
        return true;
    }
    return false;
}

bool DnsUdpSendQueue::AcquireAwaiter::await_suspend(std::coroutine_handle<> handle) noexcept {
    FIBER_ASSERT(queue_ != nullptr);
    FIBER_ASSERT(queue_->loop_ != nullptr);
    FIBER_ASSERT(queue_->loop_->in_loop());
    handle_ = handle;
    if (queue_->closed_) {
        err_ = common::IoErr::Canceled;
        handle_ = {};
        return false;
    }
    if (queue_->state_ == State::Idle) {
        FIBER_ASSERT(queue_->head_ == nullptr);
        FIBER_ASSERT(queue_->tail_ == nullptr);
        queue_->state_ = State::Owned;
        granted_ = true;
        handle_ = {};
        return false;
    }
    queue_->enqueue(*this);
    return true;
}

common::IoResult<DnsUdpSendQueue::Owner> DnsUdpSendQueue::AcquireAwaiter::await_resume() noexcept {
    handle_ = {};
    FIBER_ASSERT(!queued_);
    DnsUdpSendQueue *queue = std::exchange(queue_, nullptr);
    if (err_ != common::IoErr::None) {
        return std::unexpected(err_);
    }
    FIBER_ASSERT(queue != nullptr);
    FIBER_ASSERT(granted_);
    granted_ = false;
    return Owner(*queue);
}

void DnsUdpSendQueue::enqueue(AcquireAwaiter &awaiter) noexcept {
    FIBER_ASSERT(loop_ != nullptr);
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(state_ != State::Idle);
    FIBER_ASSERT(!awaiter.queued_);
    awaiter.prev_ = tail_;
    awaiter.next_ = nullptr;
    if (tail_) {
        tail_->next_ = &awaiter;
    } else {
        head_ = &awaiter;
    }
    tail_ = &awaiter;
    awaiter.queued_ = true;
}

void DnsUdpSendQueue::unlink(AcquireAwaiter &awaiter) noexcept {
    FIBER_ASSERT(loop_ != nullptr);
    FIBER_ASSERT(loop_->in_loop());
    if (!awaiter.queued_) {
        return;
    }
    if (awaiter.prev_) {
        awaiter.prev_->next_ = awaiter.next_;
    } else {
        head_ = awaiter.next_;
    }
    if (awaiter.next_) {
        awaiter.next_->prev_ = awaiter.prev_;
    } else {
        tail_ = awaiter.prev_;
    }
    awaiter.prev_ = nullptr;
    awaiter.next_ = nullptr;
    awaiter.queued_ = false;

    if (state_ == State::Handoff && head_ == nullptr) {
        if (handoff_entry_.is_in_queue()) {
            loop_->cancel<DnsUdpSendQueue, &DnsUdpSendQueue::handoff_entry_>(*this);
        }
        state_ = State::Idle;
    }
}

DnsUdpSendQueue::AcquireAwaiter *DnsUdpSendQueue::pop_front() noexcept {
    AcquireAwaiter *awaiter = head_;
    if (!awaiter) {
        return nullptr;
    }
    head_ = awaiter->next_;
    if (head_) {
        head_->prev_ = nullptr;
    } else {
        tail_ = nullptr;
    }
    awaiter->prev_ = nullptr;
    awaiter->next_ = nullptr;
    awaiter->queued_ = false;
    return awaiter;
}

void DnsUdpSendQueue::finish_owner() noexcept {
    FIBER_ASSERT(loop_ != nullptr);
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(state_ == State::Owned);
    if (closed_ || head_ == nullptr) {
        state_ = State::Idle;
        return;
    }
    state_ = State::Handoff;
    schedule_handoff();
}

void DnsUdpSendQueue::schedule_handoff() noexcept {
    FIBER_ASSERT(loop_ != nullptr);
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(state_ == State::Handoff);
    loop_->post_local<DnsUdpSendQueue, &DnsUdpSendQueue::handoff_entry_, &DnsUdpSendQueue::on_handoff>(*this);
}

void DnsUdpSendQueue::on_handoff(DnsUdpSendQueue *queue) noexcept {
    if (!queue) {
        return;
    }
    FIBER_ASSERT(queue->loop_ != nullptr);
    FIBER_ASSERT(queue->loop_->in_loop());
    if (queue->closed_) {
        queue->state_ = State::Idle;
        return;
    }
    FIBER_ASSERT(queue->state_ == State::Handoff);
    AcquireAwaiter *awaiter = queue->pop_front();
    if (!awaiter) {
        queue->state_ = State::Idle;
        return;
    }

    queue->state_ = State::Owned;
    awaiter->granted_ = true;
    auto handle = awaiter->handle_;
    awaiter->handle_ = {};
    if (handle) {
        handle.resume();
    }
}

} // namespace fiber::dns::detail
