#include <fiber/async/LocalWaitGroup.h>

#include <fiber/common/Assert.h>
#include <fiber/event/EventLoop.h>

namespace fiber::async {

bool LocalWaitGroup::JoinAwaiter::await_ready() noexcept {
    if (group_->count_ == 0) {
        set_result(common::IoErr::None);
        mark_completed();
        return true;
    }
    return false;
}

bool LocalWaitGroup::JoinAwaiter::await_suspend(std::coroutine_handle<> handle) noexcept {
    if (group_->count_ == 0) {
        set_result(common::IoErr::None);
        mark_completed();
        return false;
    }
    event::EventLoop *loop = event::EventLoop::current_or_null();
    FIBER_ASSERT(loop != nullptr);
    group_->link(*this);
    begin_wait(handle, *loop);
    return true;
}

void LocalWaitGroup::JoinAwaiter::await_resume() noexcept {
    end_wait();
    detach();
}

void LocalWaitGroup::JoinAwaiter::detach_from_group(WaitAwaiter &base) noexcept {
    auto &self = static_cast<JoinAwaiter &>(base);
    if (self.linked_) {
        self.group_->unlink(self);
    }
}

LocalWaitGroup::~LocalWaitGroup() {
    FIBER_ASSERT(count_ == 0);
    FIBER_ASSERT(head_ == nullptr && tail_ == nullptr);
}

void LocalWaitGroup::add(std::size_t n) noexcept { count_ += n; }

void LocalWaitGroup::done() noexcept {
    FIBER_ASSERT(count_ != 0);
    if (--count_ != 0) {
        return;
    }
    // complete() unlinks, so read the next waiter first.
    JoinAwaiter *waiter = head_;
    while (waiter != nullptr) {
        JoinAwaiter *next = waiter->next_;
        waiter->complete(common::IoErr::None);
        waiter = next;
    }
}

void LocalWaitGroup::link(JoinAwaiter &waiter) noexcept {
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
}

void LocalWaitGroup::unlink(JoinAwaiter &waiter) noexcept {
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
}

} // namespace fiber::async
