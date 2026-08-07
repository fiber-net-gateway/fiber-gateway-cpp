#include <gtest/gtest.h>

#include <atomic>
#include <coroutine>
#include <future>
#include <thread>

#include <fiber/async/CoroutinePromiseBase.h>
#include <fiber/async/Spawn.h>
#include <fiber/async/Yield.h>
#include <fiber/event/EventLoopGroup.h>

namespace {

using DetachedTask = fiber::async::DetachedTask;

struct YieldResult {
    bool same_loop = false;
};

DetachedTask run_yield(std::promise<YieldResult> *done) {
    auto *before = &fiber::event::EventLoop::current();
    co_await fiber::async::yield();
    done->set_value({.same_loop = before == &fiber::event::EventLoop::current()});
    fiber::event::EventLoop::current().stop();
}

class ManualTask {
public:
    struct promise_type : fiber::async::CoroutinePromiseBase {
        ManualTask get_return_object() { return ManualTask{handle_type::from_promise(*this)}; }

        std::suspend_always initial_suspend() noexcept { return {}; }

        std::suspend_always final_suspend() noexcept { return {}; }

        void return_void() noexcept {}

        void unhandled_exception() { std::terminate(); }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    explicit ManualTask(handle_type handle) : handle_(handle) {}

    ManualTask(const ManualTask &) = delete;
    ManualTask &operator=(const ManualTask &) = delete;

    ManualTask(ManualTask &&other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }

    ManualTask &operator=(ManualTask &&other) noexcept {
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

    ~ManualTask() {
        if (handle_) {
            handle_.destroy();
        }
    }

    handle_type release() noexcept {
        handle_type out = handle_;
        handle_ = nullptr;
        return out;
    }

private:
    handle_type handle_{};
};

ManualTask run_yield_cancel(std::atomic<int> *hits) {
    co_await fiber::async::yield();
    hits->fetch_add(1, std::memory_order_relaxed);
    co_return;
}

} // namespace

TEST(YieldTest, ResumesOnSameLoop) {
    fiber::event::EventLoopGroup group(1);
    std::promise<YieldResult> done;
    auto future = done.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&done]() { return run_yield(&done); });

    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "yield did not resume in time";
        return;
    }

    YieldResult result = future.get();
    EXPECT_TRUE(result.same_loop);
    group.join();
}

TEST(YieldTest, CancelOnDestroy) {
    fiber::event::EventLoopGroup group(1);
    std::promise<void> ready;
    auto future = ready.get_future();
    std::atomic<int> hits{0};

    group.start();
    fiber::async::spawn(group.at(0), [&ready, &hits]() -> DetachedTask {
        auto task = run_yield_cancel(&hits);
        auto handle = task.release();
        handle.resume();
        handle.destroy();
        ready.set_value();
        co_return;
    });

    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "loop did not execute cancellation in time";
        return;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    group.stop();
    group.join();
    EXPECT_EQ(hits.load(std::memory_order_relaxed), 0);
}
