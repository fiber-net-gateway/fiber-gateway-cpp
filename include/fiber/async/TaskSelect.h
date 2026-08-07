#ifndef FIBER_ASYNC_TASK_SELECT_H
#define FIBER_ASYNC_TASK_SELECT_H

#include <coroutine>
#include <type_traits>
#include <utility>

#include "../common/Assert.h"
#include "../event/EventLoop.h"
#include "Task.h"

namespace fiber::async {

template<typename T>
class TaskSelectAwaiter {
public:
    using TaskType = Task<T>;
    using handle_type = typename TaskType::handle_type;

    TaskSelectAwaiter(const TaskSelectAwaiter &) = delete;
    TaskSelectAwaiter &operator=(const TaskSelectAwaiter &) = delete;
    TaskSelectAwaiter(TaskSelectAwaiter &&) = delete;
    TaskSelectAwaiter &operator=(TaskSelectAwaiter &&) = delete;

    ~TaskSelectAwaiter() noexcept {
        if (start_armed_) {
            FIBER_ASSERT(loop_ != nullptr);
            loop_->cancel<TaskSelectAwaiter, &TaskSelectAwaiter::start_entry_>(*this);
        }
        handle_.destroy();
    }

    bool await_ready() const noexcept { return handle_.done(); }

    bool await_suspend(std::coroutine_handle<> continuation) noexcept {
        FIBER_ASSERT(!handle_.done());
        loop_ = &event::EventLoop::current();
        FIBER_ASSERT(loop_->in_loop());
        handle_.promise().set_continuation(continuation);
        start_armed_ = true;
        loop_->post_local<TaskSelectAwaiter, &TaskSelectAwaiter::start_entry_, &TaskSelectAwaiter::on_start>(*this);
        return true;
    }

    T await_resume() {
        FIBER_ASSERT(completed());
        if constexpr (std::is_void_v<T>) {
            handle_.promise().result();
        } else {
            return handle_.promise().result();
        }
    }

    [[nodiscard]] bool completed() const noexcept { return handle_.done(); }

private:
    friend class Task<T>;

    explicit TaskSelectAwaiter(handle_type handle) noexcept : handle_(handle) { FIBER_ASSERT(handle_); }

    static void on_start(TaskSelectAwaiter *awaiter) noexcept {
        FIBER_ASSERT(awaiter != nullptr);
        awaiter->start_armed_ = false;
        auto handle = awaiter->handle_;
        FIBER_ASSERT(handle);
        FIBER_ASSERT(!handle.done());
        handle.resume();
    }

    handle_type handle_{};
    fiber::event::EventLoop *loop_ = nullptr;
    fiber::event::EventLoop::DeferEntry start_entry_{};
    bool start_armed_ = false;
};

template<typename T>
TaskSelectAwaiter<T> Task<T>::select() && noexcept {
    auto handle = std::exchange(handle_, nullptr);
    return TaskSelectAwaiter<T>(handle);
}

inline TaskSelectAwaiter<void> Task<void>::select() && noexcept {
    auto handle = std::exchange(handle_, nullptr);
    return TaskSelectAwaiter<void>(handle);
}

} // namespace fiber::async

#endif // FIBER_ASYNC_TASK_SELECT_H
