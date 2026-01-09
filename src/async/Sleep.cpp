#include "Sleep.h"

#include <chrono>

#include "../event/EventLoop.h"

namespace fiber::async {

SleepAwaiter::SleepAwaiter(std::chrono::steady_clock::duration delay)
    : delay_(delay) {
    timer_.callback = &SleepAwaiter::on_timer;
    timer_.owner = this;
}

SleepAwaiter::~SleepAwaiter() {
    if (!armed_ || !loop_) {
        return;
    }
    loop_->cancel(timer_);
}

bool SleepAwaiter::await_ready() const noexcept {
    return delay_ <= std::chrono::steady_clock::duration::zero();
}

void SleepAwaiter::await_suspend(std::coroutine_handle<> handle) {
    handle_ = handle;
    armed_ = true;
    loop_ = &event::EventLoop::current();
    loop_->post_at(std::chrono::steady_clock::now() + delay_, timer_);
}

void SleepAwaiter::on_timer(fiber::event::EventLoop::TimerEntry *entry) {
    auto *timer = static_cast<SleepTimer *>(entry);
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
