#include "Yield.h"

namespace fiber::async {

YieldAwaiter::~YieldAwaiter() {
    if (!armed_ || !loop_) {
        return;
    }
    loop_->cancel<YieldAwaiter, &YieldAwaiter::entry_>(*this);
}

bool YieldAwaiter::await_suspend(std::coroutine_handle<> handle) noexcept {
    loop_ = &event::EventLoop::current();
    FIBER_ASSERT(loop_->in_loop());
    handle_ = handle;
    armed_ = true;
    loop_->post_local<YieldAwaiter, &YieldAwaiter::entry_, &YieldAwaiter::on_yield>(*this);
    return true;
}

void YieldAwaiter::on_yield(YieldAwaiter *awaiter) noexcept {
    if (!awaiter) {
        return;
    }
    awaiter->armed_ = false;
    auto handle = awaiter->handle_;
    awaiter->handle_ = {};
    if (handle) {
        handle.resume();
    }
}

YieldAwaiter yield() noexcept { return YieldAwaiter{}; }

} // namespace fiber::async
