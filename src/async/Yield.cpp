#include <fiber/async/Yield.h>

namespace fiber::async {

YieldAwaiter::~YieldAwaiter() {
    if (!armed_ || !loop_) {
        return;
    }
    loop_->cancel<YieldAwaiter, &YieldAwaiter::entry_>(*this);
}

bool YieldAwaiter::await_suspend(std::coroutine_handle<> handle) noexcept {
    FIBER_ASSERT(!completed_);
    loop_ = &event::EventLoop::current();
    FIBER_ASSERT(loop_->in_loop());
    handle_ = handle;
    armed_ = true;
    // Yield resumes after the next poll so pending kernel events, timers and
    // other turns run in between.
    loop_->post_next<YieldAwaiter, &YieldAwaiter::entry_, &YieldAwaiter::on_yield>(*this);
    return true;
}

void YieldAwaiter::await_resume() noexcept {
    FIBER_ASSERT(completed_);
    handle_ = {};
}

void YieldAwaiter::on_yield(YieldAwaiter *awaiter) noexcept {
    if (!awaiter) {
        return;
    }
    awaiter->armed_ = false;
    awaiter->completed_ = true;
    auto handle = awaiter->handle_;
    awaiter->handle_ = {};
    if (handle) {
        handle.resume();
    }
}

YieldAwaiter yield() noexcept { return YieldAwaiter{}; }

} // namespace fiber::async
