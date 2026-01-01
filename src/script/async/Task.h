#ifndef FIBER_SCRIPT_ASYNC_TASK_H
#define FIBER_SCRIPT_ASYNC_TASK_H

#include <concepts>
#include <coroutine>
#include <expected>
#include <optional>
#include <string>
#include <utility>

namespace fiber::script::async {

struct IScheduler {
    virtual void post(std::coroutine_handle<> handle) = 0;
    virtual ~IScheduler() = default;
};

struct InlineScheduler final : IScheduler {
    void post(std::coroutine_handle<> handle) override {
        handle.resume();
    }
};

class TaskPromiseBase {
public:
    std::suspend_always initial_suspend() noexcept {
        return {};
    }

    struct FinalAwaiter {
        bool await_ready() noexcept {
            return false;
        }

        template <typename Promise>
        void await_suspend(std::coroutine_handle<Promise> handle) noexcept {
            Promise &promise = handle.promise();
            std::coroutine_handle<> cont = promise.continuation();
            if (!cont) {
                return;
            }
            IScheduler *scheduler = promise.scheduler();
            if (scheduler) {
                scheduler->post(cont);
            } else {
                cont.resume();
            }
        }

        void await_resume() noexcept {
        }
    };

    FinalAwaiter final_suspend() noexcept {
        return {};
    }

    void set_scheduler(IScheduler *scheduler) {
        scheduler_ = scheduler;
    }

    IScheduler *scheduler() const {
        return scheduler_;
    }

    void set_continuation(std::coroutine_handle<> handle) {
        continuation_ = handle;
    }

    std::coroutine_handle<> continuation() const {
        return continuation_;
    }

private:
    IScheduler *scheduler_ = nullptr;
    std::coroutine_handle<> continuation_ = nullptr;
};

struct TaskError {
    std::string message;
};

template <typename T>
class Task {
public:
    struct promise_type : TaskPromiseBase {
        std::optional<std::expected<T, TaskError>> result_;

        Task get_return_object() {
            return Task{handle_type::from_promise(*this)};
        }

        void unhandled_exception() {
            result_ = std::unexpected(TaskError{"unhandled exception"});
        }

        template <typename U>
            requires std::convertible_to<U, T>
        void return_value(U &&value) {
            result_ = std::expected<T, TaskError>(std::in_place, std::forward<U>(value));
        }

        void return_value(std::expected<T, TaskError> value) {
            result_ = std::move(value);
        }

        std::expected<T, TaskError> result() {
            if (!result_) {
                return std::unexpected(TaskError{"no result"});
            }
            return std::move(*result_);
        }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    Task() = default;
    explicit Task(handle_type handle) : handle_(handle) {
    }

    Task(const Task &) = delete;
    Task &operator=(const Task &) = delete;

    Task(Task &&other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    Task &operator=(Task &&other) noexcept {
        if (this == &other) {
            return *this;
        }
        if (handle_) {
            handle_.destroy();
        }
        handle_ = other.handle_;
        other.handle_ = nullptr;
        return *this;
    }

    ~Task() {
        if (handle_) {
            handle_.destroy();
        }
    }

    bool valid() const {
        return static_cast<bool>(handle_);
    }

    void set_scheduler(IScheduler *scheduler) {
        if (handle_) {
            handle_.promise().set_scheduler(scheduler);
        }
    }

    struct Awaiter {
        handle_type handle;

        bool await_ready() const noexcept {
            return !handle || handle.done();
        }

        void await_suspend(std::coroutine_handle<> cont) {
            handle.promise().set_continuation(cont);
            IScheduler *scheduler = handle.promise().scheduler();
            if (scheduler) {
                scheduler->post(handle);
            } else {
                handle.resume();
            }
        }

        std::expected<T, TaskError> await_resume() {
            return handle.promise().result();
        }
    };

    Awaiter operator co_await() {
        return Awaiter{handle_};
    }

private:
    handle_type handle_ = nullptr;
};

template <>
class Task<void> {
public:
    struct promise_type : TaskPromiseBase {
        std::optional<std::expected<void, TaskError>> result_;

        Task get_return_object() {
            return Task{handle_type::from_promise(*this)};
        }

        void unhandled_exception() {
            result_ = std::unexpected(TaskError{"unhandled exception"});
        }

        void return_value(std::expected<void, TaskError> value) {
            result_ = std::move(value);
        }

        std::expected<void, TaskError> result() {
            if (!result_) {
                return std::unexpected(TaskError{"no result"});
            }
            return std::move(*result_);
        }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    Task() = default;
    explicit Task(handle_type handle) : handle_(handle) {
    }

    Task(const Task &) = delete;
    Task &operator=(const Task &) = delete;

    Task(Task &&other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    Task &operator=(Task &&other) noexcept {
        if (this == &other) {
            return *this;
        }
        if (handle_) {
            handle_.destroy();
        }
        handle_ = other.handle_;
        other.handle_ = nullptr;
        return *this;
    }

    ~Task() {
        if (handle_) {
            handle_.destroy();
        }
    }

    bool valid() const {
        return static_cast<bool>(handle_);
    }

    void set_scheduler(IScheduler *scheduler) {
        if (handle_) {
            handle_.promise().set_scheduler(scheduler);
        }
    }

    struct Awaiter {
        handle_type handle;

        bool await_ready() const noexcept {
            return !handle || handle.done();
        }

        void await_suspend(std::coroutine_handle<> cont) {
            handle.promise().set_continuation(cont);
            IScheduler *scheduler = handle.promise().scheduler();
            if (scheduler) {
                scheduler->post(handle);
            } else {
                handle.resume();
            }
        }

        std::expected<void, TaskError> await_resume() {
            return handle.promise().result();
        }
    };

    Awaiter operator co_await() {
        return Awaiter{handle_};
    }

private:
    handle_type handle_ = nullptr;
};

template <typename T>
class TaskCompletionSource {
public:
    TaskCompletionSource() = default;

    Task<T> task() {
        return wait();
    }

    void set_value(T value) {
        state_.set_result(std::expected<T, TaskError>(std::in_place, std::move(value)));
    }

    void set_error(TaskError error) {
        state_.set_result(std::unexpected(std::move(error)));
    }

    void set_scheduler(IScheduler *scheduler) {
        state_.scheduler = scheduler;
    }

private:
    struct State {
        std::optional<std::expected<T, TaskError>> result;
        bool ready = false;
        std::coroutine_handle<> continuation = nullptr;
        IScheduler *scheduler = nullptr;

        void set_result(std::expected<T, TaskError> value) {
            if (ready) {
                return;
            }
            result = std::move(value);
            ready = true;
            resume();
        }

        void resume() {
            if (!continuation) {
                return;
            }
            if (scheduler) {
                scheduler->post(continuation);
            } else {
                continuation.resume();
            }
        }
    };

    struct Awaiter {
        State *state;

        bool await_ready() const noexcept {
            return state->ready;
        }

        void await_suspend(std::coroutine_handle<> handle) {
            state->continuation = handle;
        }

        std::expected<T, TaskError> await_resume() {
            if (!state->result) {
                return std::unexpected(TaskError{"no result"});
            }
            return std::move(*state->result);
        }
    };

    Task<T> wait() {
        co_return co_await Awaiter{&state_};
    }

    State state_;
};

template <>
class TaskCompletionSource<void> {
public:
    TaskCompletionSource() = default;

    Task<void> task() {
        return wait();
    }

    void set_value() {
        state_.set_result(std::expected<void, TaskError>());
    }

    void set_error(TaskError error) {
        state_.set_result(std::unexpected(std::move(error)));
    }

    void set_scheduler(IScheduler *scheduler) {
        state_.scheduler = scheduler;
    }

private:
    struct State {
        std::optional<std::expected<void, TaskError>> result;
        bool ready = false;
        std::coroutine_handle<> continuation = nullptr;
        IScheduler *scheduler = nullptr;

        void set_result(std::expected<void, TaskError> value) {
            if (ready) {
                return;
            }
            ready = true;
            result = std::move(value);
            resume();
        }

        void resume() {
            if (!continuation) {
                return;
            }
            if (scheduler) {
                scheduler->post(continuation);
            } else {
                continuation.resume();
            }
        }
    };

    struct Awaiter {
        State *state;

        bool await_ready() const noexcept {
            return state->ready;
        }

        void await_suspend(std::coroutine_handle<> handle) {
            state->continuation = handle;
        }

        std::expected<void, TaskError> await_resume() {
            if (!state->result) {
                return std::unexpected(TaskError{"no result"});
            }
            return std::move(*state->result);
        }
    };

    Task<void> wait() {
        co_return co_await Awaiter{&state_};
    }

    State state_;
};

} // namespace fiber::script::async

#endif // FIBER_SCRIPT_ASYNC_TASK_H
