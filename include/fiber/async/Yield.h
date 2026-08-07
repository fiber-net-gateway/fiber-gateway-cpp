#ifndef FIBER_ASYNC_YIELD_H
#define FIBER_ASYNC_YIELD_H

#include <coroutine>

#include "../common/Assert.h"
#include "../event/EventLoop.h"

namespace fiber::async {

class YieldAwaiter {
public:
    YieldAwaiter() noexcept = default;
    YieldAwaiter(const YieldAwaiter &) = delete;
    YieldAwaiter &operator=(const YieldAwaiter &) = delete;
    YieldAwaiter(YieldAwaiter &&other) noexcept : completed_(other.completed_) {
        FIBER_ASSERT(!other.armed_);
        other.loop_ = nullptr;
        other.handle_ = {};
        other.armed_ = false;
        other.completed_ = false;
    }
    YieldAwaiter &operator=(YieldAwaiter &&) = delete;
    ~YieldAwaiter();

    bool await_ready() const noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> handle) noexcept;
    void await_resume() noexcept;
    [[nodiscard]] bool completed() const noexcept { return completed_; }

private:
    static void on_yield(YieldAwaiter *awaiter) noexcept;

    fiber::event::EventLoop *loop_ = nullptr;
    std::coroutine_handle<> handle_{};
    fiber::event::EventLoop::DeferEntry entry_{};
    bool armed_ = false;
    bool completed_ = false;
};

YieldAwaiter yield() noexcept;

} // namespace fiber::async

#endif // FIBER_ASYNC_YIELD_H
