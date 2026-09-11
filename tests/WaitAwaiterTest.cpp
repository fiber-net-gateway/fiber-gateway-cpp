#include <gtest/gtest.h>

#include <chrono>
#include <coroutine>
#include <optional>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/async/WaitAwaiter.h>
#include <fiber/event/EventLoop.h>

namespace {

using fiber::common::IoErr;

// Minimal user of the primitive: one queue slot, the shape every real awaiter
// in the tree has. The slot doubles as a detach witness, so the tests can check
// that a completion leaves no dangling link.
class TestWaiter : public fiber::async::WaitAwaiter {
public:
    TestWaiter(TestWaiter **slot, std::chrono::steady_clock::time_point deadline) noexcept :
        WaitAwaiter(deadline, &TestWaiter::detach_from_slot), slot_(slot) {
        *slot_ = this;
    }

    ~TestWaiter() { detach(); }

    [[nodiscard]] bool await_ready() const noexcept { return completed() || signaled_; }

    bool await_suspend(std::coroutine_handle<> handle) noexcept {
        begin_wait(handle, fiber::event::EventLoop::current());
        return true;
    }

    IoErr await_resume() noexcept {
        signaled_ = false;
        return result();
    }

    // Non-terminal wake: stays queued, as the stream gates' waiters do.
    void signal_available() noexcept {
        if (signaled_ || completed()) {
            return;
        }
        signaled_ = true;
        post_resume();
    }

private:
    static void detach_from_slot(fiber::async::WaitAwaiter &base) noexcept {
        auto &self = static_cast<TestWaiter &>(base);
        if (self.slot_ != nullptr && *self.slot_ == &self) {
            *self.slot_ = nullptr;
        }
    }

    TestWaiter **slot_ = nullptr;
    bool signaled_ = false;
};

struct Outcome {
    std::optional<IoErr> value;
    int resumes = 0;
};

// A coroutine whose frame the test owns outright, so it can be destroyed
// mid-flight the way when_any discards a task it no longer needs.
struct ParkTask {
    struct promise_type {
        ParkTask get_return_object() noexcept {
            return ParkTask{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() { FIBER_PANIC("unhandled exception in ParkTask"); }
    };

    ParkTask(const ParkTask &) = delete;
    ParkTask &operator=(const ParkTask &) = delete;
    explicit ParkTask(std::coroutine_handle<promise_type> handle) noexcept : handle_(handle) {}
    ~ParkTask() {
        if (handle_) {
            handle_.destroy();
        }
    }

    void start() { handle_.resume(); }

    std::coroutine_handle<promise_type> handle_{};
};

// The awaiter lives inside the frame, so destroying the frame destroys it.
ParkTask park_once(TestWaiter **slot, std::chrono::steady_clock::time_point deadline, Outcome *outcome) {
    TestWaiter waiter(slot, deadline);
    const IoErr result = co_await waiter;
    ++outcome->resumes;
    outcome->value = result;
}

ParkTask park_twice(TestWaiter **slot, std::chrono::steady_clock::time_point deadline, Outcome *outcome) {
    TestWaiter waiter(slot, deadline);
    for (;;) {
        const IoErr result = co_await waiter;
        ++outcome->resumes;
        outcome->value = result;
        if (result != IoErr::None) {
            co_return;
        }
    }
}

} // namespace

TEST(WaitAwaiterTest, CompleteResumesOnceAndDetaches) {
    fiber::event::EventLoop loop;
    TestWaiter *slot = nullptr;
    Outcome outcome;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        ParkTask task = park_once(&slot, std::chrono::steady_clock::time_point::max(), &outcome);
        task.start();
        EXPECT_NE(slot, nullptr);
        if (slot == nullptr) {
            fiber::event::EventLoop::current().stop();
            co_return;
        }

        TestWaiter *waiter = slot;
        waiter->complete(IoErr::ConnReset);
        // complete() detaches, so the slot is free before the coroutine has run.
        EXPECT_EQ(slot, nullptr);
        // A second result must be ignored, not queue another resume.
        waiter->complete(IoErr::BrokenPipe);

        co_await fiber::async::sleep(std::chrono::milliseconds(10));
        fiber::event::EventLoop::current().stop();
    });

    loop.run();

    ASSERT_TRUE(outcome.value.has_value());
    EXPECT_EQ(*outcome.value, IoErr::ConnReset);
    EXPECT_EQ(outcome.resumes, 1);
}

TEST(WaitAwaiterTest, DeadlineCompletesWithTimedOut) {
    fiber::event::EventLoop loop;
    TestWaiter *slot = nullptr;
    Outcome outcome;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        ParkTask task =
                park_once(&slot, fiber::event::EventLoop::current().now() + std::chrono::milliseconds(10), &outcome);
        task.start();
        co_await fiber::async::sleep(std::chrono::milliseconds(60));
        fiber::event::EventLoop::current().stop();
    });

    loop.run();

    ASSERT_TRUE(outcome.value.has_value());
    EXPECT_EQ(*outcome.value, IoErr::TimedOut);
    EXPECT_EQ(outcome.resumes, 1);
    EXPECT_EQ(slot, nullptr);
}

// The reason resumes go through the loop's cancellable local defer queue rather
// than the MPSC notify queue: an awaiter can be destroyed while its resume is
// still queued, and an MPSC entry cannot be retracted -- the loop would later
// pop it and call into freed memory.
TEST(WaitAwaiterTest, DestroyingAwaiterRetractsAQueuedResume) {
    fiber::event::EventLoop loop;
    TestWaiter *slot = nullptr;
    Outcome outcome;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        {
            ParkTask task = park_once(&slot, std::chrono::steady_clock::time_point::max(), &outcome);
            task.start();
            EXPECT_NE(slot, nullptr);
            if (slot != nullptr) {
                slot->complete(IoErr::Canceled);
            }
            // Drop the frame, and the awaiter inside it, before the loop runs
            // the resume it just queued.
        }
        EXPECT_EQ(slot, nullptr);
        co_await fiber::async::sleep(std::chrono::milliseconds(10));
        fiber::event::EventLoop::current().stop();
    });

    loop.run();

    // The resume went with the awaiter: nothing ran, nothing touched freed memory.
    EXPECT_EQ(outcome.resumes, 0);
    EXPECT_FALSE(outcome.value.has_value());
}

// A waiter that parks again after a non-terminal wake keeps the deadline it
// already has. Re-arming would restart it, and would trip the loop's
// one-entry-per-timer invariant.
TEST(WaitAwaiterTest, ReParkingKeepsTheRunningDeadline) {
    fiber::event::EventLoop loop;
    TestWaiter *slot = nullptr;
    Outcome outcome;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        auto &current = fiber::event::EventLoop::current();
        ParkTask task = park_twice(&slot, current.now() + std::chrono::milliseconds(40), &outcome);
        task.start();
        EXPECT_NE(slot, nullptr);

        // Wake without completing: the coroutine re-parks on the same awaiter.
        co_await fiber::async::sleep(std::chrono::milliseconds(10));
        EXPECT_NE(slot, nullptr);
        if (slot != nullptr) {
            slot->signal_available();
        }

        co_await fiber::async::sleep(std::chrono::milliseconds(90));
        current.stop();
    });

    loop.run();

    // One resume for the signal, one for the deadline that kept running.
    EXPECT_EQ(outcome.resumes, 2);
    ASSERT_TRUE(outcome.value.has_value());
    EXPECT_EQ(*outcome.value, IoErr::TimedOut);
    EXPECT_EQ(slot, nullptr);
}
