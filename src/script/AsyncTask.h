#ifndef FIBER_SCRIPT_ASYNC_TASK_H
#define FIBER_SCRIPT_ASYNC_TASK_H

#include <coroutine>
#include <cstddef>
#include <new>

#include "../common/Assert.h"
#include "ScriptResult.h"

namespace fiber::script {

struct AsyncTaskCompletion {
    void (*complete)(void *ctx, const ScriptResult &result) noexcept = nullptr;
    void *ctx = nullptr;
};

class AsyncTask {
public:
    struct promise_type {
        using handle_type = std::coroutine_handle<promise_type>;

        static void *operator new(std::size_t size) noexcept { return ::operator new(size, std::nothrow); }

        static void operator delete(void *ptr, std::size_t size) noexcept { ::operator delete(ptr, size); }

        static AsyncTask get_return_object_on_allocation_failure() noexcept {
            AsyncTask task;
            task.allocation_failed_ = true;
            return task;
        }

        AsyncTask get_return_object() noexcept { return AsyncTask{handle_type::from_promise(*this)}; }

        std::suspend_always initial_suspend() noexcept { return {}; }

        struct FinalAwaiter {
            bool await_ready() noexcept { return false; }

            std::coroutine_handle<> await_suspend(handle_type handle) noexcept {
                promise_type &promise = handle.promise();
                std::coroutine_handle<> continuation = promise.continuation_;
                return continuation ? continuation : std::noop_coroutine();
            }

            void await_resume() noexcept {}
        };

        FinalAwaiter final_suspend() noexcept { return {}; }

        void return_value(const ScriptResult &result) noexcept {
            result_ = result;
            if (completion_.complete) {
                completion_.complete(completion_.ctx, result_);
            }
        }

        void unhandled_exception() noexcept { FIBER_PANIC("unhandled exception in script AsyncTask"); }

        void set_continuation(std::coroutine_handle<> continuation) noexcept { continuation_ = continuation; }

        void set_completion(AsyncTaskCompletion completion) noexcept { completion_ = completion; }

        ScriptResult result_ = ScriptResult::abort(ScriptAbortReason::InvalidState);
        AsyncTaskCompletion completion_{};
        std::coroutine_handle<> continuation_ = nullptr;
    };

    using handle_type = std::coroutine_handle<promise_type>;

    AsyncTask() = default;

    explicit AsyncTask(handle_type handle) noexcept : handle_(handle) {}

    AsyncTask(const AsyncTask &) = delete;
    AsyncTask &operator=(const AsyncTask &) = delete;

    AsyncTask(AsyncTask &&other) noexcept : handle_(other.handle_), allocation_failed_(other.allocation_failed_) {
        other.handle_ = nullptr;
        other.allocation_failed_ = false;
    }

    AsyncTask &operator=(AsyncTask &&other) noexcept {
        if (this == &other) {
            return *this;
        }
        if (handle_) {
            handle_.destroy();
        }
        handle_ = other.handle_;
        allocation_failed_ = other.allocation_failed_;
        other.handle_ = nullptr;
        other.allocation_failed_ = false;
        return *this;
    }

    ~AsyncTask() {
        if (handle_) {
            handle_.destroy();
        }
    }

    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(handle_); }

    [[nodiscard]] bool allocation_failed() const noexcept { return allocation_failed_; }

    void reset() noexcept {
        if (handle_) {
            handle_.destroy();
            handle_ = nullptr;
        }
        allocation_failed_ = false;
    }

    void set_completion(AsyncTaskCompletion completion) noexcept {
        if (handle_) {
            handle_.promise().set_completion(completion);
        }
    }

    std::coroutine_handle<> swap_coroutine_handle(std::coroutine_handle<> handle) noexcept {
        if (!handle_) {
            return std::noop_coroutine();
        }
        handle_.promise().set_continuation(handle);
        return handle_;
    }

private:
    handle_type handle_ = nullptr;
    bool allocation_failed_ = false;
};

} // namespace fiber::script

#endif // FIBER_SCRIPT_ASYNC_TASK_H
