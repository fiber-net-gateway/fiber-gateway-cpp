#include <fiber/async/WaitAwaiter.h>

#include <utility>

#include <fiber/common/Assert.h>

namespace fiber::async {

void WaitAwaiter::complete(common::IoErr result) noexcept {
    if (completed_) {
        return;
    }
    completed_ = true;
    result_ = result;
    cancel_timer();
    detach();
    post_resume();
}

void WaitAwaiter::begin_wait(std::coroutine_handle<> handle, event::EventLoop &loop) noexcept {
    FIBER_ASSERT(loop_ == nullptr || loop_ == &loop);
    handle_ = handle;
    loop_ = &loop;
    // The deadline is absolute, so a waiter that parks again after a
    // non-terminal wake keeps the timer it already has rather than restarting
    // it. Re-arming would also trip the loop's one-entry-per-timer invariant.
    if (!timer_entry_.is_in_heap()) {
        arm_timer();
    }
}

void WaitAwaiter::end_wait() noexcept {
    cancel_resume();
    cancel_timer();
    handle_ = {};
    loop_ = nullptr;
}

void WaitAwaiter::detach() noexcept {
    if (detach_ != nullptr) {
        detach_(*this);
    }
}

void WaitAwaiter::arm_timer() noexcept {
    if (!has_timer() || loop_ == nullptr) {
        return;
    }
    loop_->post_at<WaitAwaiter, &WaitAwaiter::timer_entry_, &WaitAwaiter::on_timeout>(deadline_, *this);
}

void WaitAwaiter::cancel_timer() noexcept {
    if (loop_ != nullptr && timer_entry_.is_in_heap()) {
        loop_->cancel<WaitAwaiter, &WaitAwaiter::timer_entry_>(*this);
    }
}

void WaitAwaiter::post_resume() noexcept {
    if (resume_posted_ || loop_ == nullptr) {
        return;
    }
    FIBER_ASSERT(loop_->in_loop());
    resume_posted_ = true;
    loop_->post_local<WaitAwaiter, &WaitAwaiter::resume_entry_, &WaitAwaiter::on_notify>(*this);
}

void WaitAwaiter::cancel_resume() noexcept {
    if (loop_ != nullptr && resume_entry_.is_in_queue()) {
        loop_->cancel<WaitAwaiter, &WaitAwaiter::resume_entry_>(*this);
        resume_posted_ = false;
    }
}

void WaitAwaiter::on_notify(WaitAwaiter *awaiter) noexcept {
    FIBER_ASSERT(awaiter != nullptr);
    awaiter->resume_posted_ = false;
    std::coroutine_handle<> handle = std::exchange(awaiter->handle_, {});
    if (handle) {
        handle.resume();
    }
}

void WaitAwaiter::on_timeout(WaitAwaiter *awaiter) noexcept {
    FIBER_ASSERT(awaiter != nullptr);
    awaiter->complete(common::IoErr::TimedOut);
}

} // namespace fiber::async
