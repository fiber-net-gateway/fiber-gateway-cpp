#ifndef FIBER_ASYNC_SPAWN_H
#define FIBER_ASYNC_SPAWN_H

#include <concepts>
#include <coroutine>
#include <exception>
#include <functional>
#include <type_traits>
#include <utility>

#include "../common/Assert.h"
#include "../event/EventLoop.h"
#include "CoroutinePromiseBase.h"

namespace fiber::async {

namespace detail {

template<typename F>
struct SpawnTask;

} // namespace detail

class [[nodiscard]] DetachedTask {
public:
    struct promise_type;
    using handle_type = std::coroutine_handle<promise_type>;

    struct promise_type : CoroutinePromiseBase {
        using OnCoroutineDestroy = void (*)(void *) noexcept;

        DetachedTask get_return_object() noexcept { return DetachedTask(handle_type::from_promise(*this)); }

        std::suspend_always initial_suspend() noexcept { return {}; }

        std::suspend_never final_suspend() noexcept { return {}; }

        void return_void() noexcept {}

        void unhandled_exception() { FIBER_PANIC("error in spawn"); }

        ~promise_type() {
            if (on_coroutine_destroy_) {
                on_coroutine_destroy_(on_coroutine_destroy_ctx_);
            }
        }

    private:
        friend class DetachedTask;

        void set_on_coroutine_destroy(void *ctx, OnCoroutineDestroy callback) noexcept {
            FIBER_ASSERT(on_coroutine_destroy_ == nullptr);
            FIBER_ASSERT(callback != nullptr);
            on_coroutine_destroy_ctx_ = ctx;
            on_coroutine_destroy_ = callback;
        }

        void *on_coroutine_destroy_ctx_ = nullptr;
        OnCoroutineDestroy on_coroutine_destroy_ = nullptr;
    };

    DetachedTask(const DetachedTask &) = delete;
    DetachedTask &operator=(const DetachedTask &) = delete;

    DetachedTask(DetachedTask &&other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

    DetachedTask &operator=(DetachedTask &&other) noexcept {
        if (this == &other) {
            return *this;
        }
        if (handle_) {
            handle_.destroy();
        }
        handle_ = std::exchange(other.handle_, nullptr);
        return *this;
    }

    ~DetachedTask() {
        if (handle_) {
            handle_.destroy();
        }
    }

private:
    template<typename F>
    friend struct detail::SpawnTask;

    explicit DetachedTask(handle_type handle) noexcept : handle_(handle) {}

    void start(void *ctx, promise_type::OnCoroutineDestroy on_coroutine_destroy) noexcept {
        handle_type handle = std::exchange(handle_, nullptr);
        FIBER_ASSERT(handle != nullptr);
        handle.promise().set_on_coroutine_destroy(ctx, on_coroutine_destroy);
        handle.resume();
    }

    handle_type handle_ = nullptr;
};

template<typename F>
concept SpawnFactory =
        std::invocable<F &> && std::same_as<std::remove_cvref_t<std::invoke_result_t<F &>>, DetachedTask>;

namespace detail {

template<typename F>
struct SpawnTask {
    fiber::event::EventLoop::NotifyEntry entry{};
    F factory;

    explicit SpawnTask(F &&fn) : factory(std::forward<F>(fn)) {}

    static void run(SpawnTask *task) noexcept {
        if (!task) {
            return;
        }
        try {
            DetachedTask detached = std::invoke(task->factory);
            // start() may complete synchronously and destroy task through the promise callback.
            detached.start(task, &SpawnTask::destroy);
        } catch (...) {
            delete task;
            std::terminate();
        }
    }

private:
    static void destroy(void *ctx) noexcept { delete static_cast<SpawnTask *>(ctx); }
};

} // namespace detail

template<typename F>
    requires SpawnFactory<F>
void spawn(fiber::event::EventLoop &loop, F &&factory) {
    using Task = detail::SpawnTask<std::decay_t<F>>;
    auto *task = new Task(std::forward<F>(factory));
    loop.post<Task, &Task::entry, &Task::run>(*task);
}

template<typename F>
    requires SpawnFactory<F>
void spawn(F &&factory) {
    auto *loop = fiber::event::EventLoop::current_or_null();
    FIBER_ASSERT(loop != nullptr);
    spawn(*loop, std::forward<F>(factory));
}

} // namespace fiber::async

#endif // FIBER_ASYNC_SPAWN_H
