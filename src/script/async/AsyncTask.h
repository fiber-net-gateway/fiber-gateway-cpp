#ifndef FIBER_SCRIPT_ASYNC_ASYNC_TASK_H
#define FIBER_SCRIPT_ASYNC_ASYNC_TASK_H

#include <coroutine>
#include <cstddef>
#include <new>

#include "../../common/Assert.h"
#include "../ScriptResult.h"

namespace fiber::script {

struct AsyncTaskCompletion {
    void (*complete)(void *ctx, ScriptResult result) noexcept = nullptr;
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
                const bool detached = promise.detached_;
                if (promise.completion_.complete) {
                    promise.completion_.complete(promise.completion_.ctx, promise.result_);
                }
                if (detached) {
                    handle.destroy();
                    return std::noop_coroutine();
                }
                return continuation ? continuation : std::noop_coroutine();
            }

            void await_resume() noexcept {}
        };

        FinalAwaiter final_suspend() noexcept { return {}; }

        void return_value(ScriptResult result) noexcept {
            result_ = result;
            has_result_ = true;
        }

        void unhandled_exception() noexcept { FIBER_PANIC("unhandled exception in script AsyncTask"); }

        void set_continuation(std::coroutine_handle<> continuation) noexcept { continuation_ = continuation; }

        void set_completion(AsyncTaskCompletion completion) noexcept { completion_ = completion; }

        void detach() noexcept { detached_ = true; }

        [[nodiscard]] ScriptResult result() const noexcept {
            if (!has_result_) {
                return ScriptResult::abort(ScriptAbortReason::InvalidState);
            }
            return result_;
        }

        ScriptResult result_ = ScriptResult::abort(ScriptAbortReason::InvalidState);
        bool has_result_ = false;
        bool detached_ = false;
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

    void start_detached(AsyncTaskCompletion completion) noexcept {
        if (!handle_) {
            if (completion.complete) {
                ScriptAbortReason reason =
                        allocation_failed_ ? ScriptAbortReason::OutOfMemory : ScriptAbortReason::InvalidState;
                completion.complete(completion.ctx, ScriptResult::abort(reason));
            }
            return;
        }
        handle_.promise().set_completion(completion);
        handle_.promise().detach();
        handle_type handle = handle_;
        handle_ = nullptr;
        handle.resume();
    }

    struct Awaiter {
        handle_type handle = nullptr;

        bool await_ready() const noexcept { return !handle || handle.done(); }

        std::coroutine_handle<> await_suspend(std::coroutine_handle<> continuation) noexcept {
            handle.promise().set_continuation(continuation);
            return handle;
        }

        ScriptResult await_resume() const noexcept {
            if (!handle) {
                return ScriptResult::abort(ScriptAbortReason::InvalidState);
            }
            return handle.promise().result();
        }
    };

    Awaiter operator co_await() noexcept { return Awaiter{handle_}; }

private:
    handle_type handle_ = nullptr;
    bool allocation_failed_ = false;
};

} // namespace fiber::script

#endif // FIBER_SCRIPT_ASYNC_ASYNC_TASK_H
