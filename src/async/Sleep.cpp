#include <fiber/async/Sleep.h>

#include <chrono>

#include <fiber/event/EventLoop.h>

namespace fiber::async {

SleepAwaiter::SleepAwaiter(std::chrono::steady_clock::duration delay) : delay_(delay) { timer_.owner = this; }

SleepAwaiter::~SleepAwaiter() {
    if (!armed_ || !loop_) {
        return;
    }
    loop_->cancel<SleepTimer, &SleepTimer::entry>(timer_);
}

bool SleepAwaiter::await_ready() noexcept {
    completed_ = delay_ <= std::chrono::steady_clock::duration::zero();
    return completed_;
}

void SleepAwaiter::await_suspend(std::coroutine_handle<> handle) {
    FIBER_ASSERT(!completed_);
    handle_ = handle;
    armed_ = true;
    loop_ = &event::EventLoop::current();
    loop_->post_at<SleepTimer, &SleepTimer::entry, &SleepTimer::on_timer>(std::chrono::steady_clock::now() + delay_,
                                                                          timer_);
}

void SleepAwaiter::await_resume() noexcept {
    FIBER_ASSERT(completed_);
    handle_ = {};
}

void SleepAwaiter::SleepTimer::on_timer(SleepTimer *timer) noexcept {
    if (!timer || !timer->owner) {
        return;
    }
    timer->owner->fire();
}

void SleepAwaiter::fire() {
    armed_ = false;
    completed_ = true;
    auto handle = handle_;
    handle_ = {};
    if (handle) {
        handle.resume();
    }
}

SleepAwaiter sleep(std::chrono::steady_clock::duration delay) { return SleepAwaiter(delay); }

} // namespace fiber::async
