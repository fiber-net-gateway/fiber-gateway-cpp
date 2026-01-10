#include "Sleep.h"

#include <chrono>

#include "../event/EventLoop.h"

namespace fiber::async {

SleepAwaiter::SleepAwaiter(std::chrono::steady_clock::duration delay)
    : delay_(delay) {
    timer_.owner = this;
}

SleepAwaiter::~SleepAwaiter() {
    if (!armed_ || !loop_) {
        return;
    }
    loop_->cancel<SleepTimer, &SleepTimer::entry>(timer_);
}

bool SleepAwaiter::await_ready() const noexcept {
    return delay_ <= std::chrono::steady_clock::duration::zero();
}

void SleepAwaiter::await_suspend(std::coroutine_handle<> handle) {
    handle_ = handle;
    armed_ = true;
    loop_ = &event::EventLoop::current();
    loop_->post_at<SleepTimer, &SleepTimer::entry, &SleepTimer::on_timer>(
        std::chrono::steady_clock::now() + delay_, timer_);
}

void SleepAwaiter::SleepTimer::on_timer(SleepTimer *timer) {
    if (!timer || !timer->owner) {
        return;
    }
    timer->owner->fire();
}

void SleepAwaiter::fire() {
    armed_ = false;
    if (handle_) {
        handle_.resume();
    }
}

SleepAwaiter sleep(std::chrono::steady_clock::duration delay) {
    return SleepAwaiter(delay);
}

} // namespace fiber::async
