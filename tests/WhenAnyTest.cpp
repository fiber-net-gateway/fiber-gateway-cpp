#include <gtest/gtest.h>

#include <chrono>
#include <coroutine>
#include <future>

#include <fiber/async/Mutex.h>
#include <fiber/async/RWMutex.h>
#include <fiber/async/Signal.h>
#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/async/Task.h>
#include <fiber/async/TaskSelect.h>
#include <fiber/async/Timeout.h>
#include <fiber/async/WaitGroup.h>
#include <fiber/async/Watch.h>
#include <fiber/async/WhenAny.h>
#include <fiber/async/Yield.h>
#include <fiber/event/EventLoopGroup.h>

namespace {

using DetachedTask = fiber::async::DetachedTask;

class ImmediateIntAwaiter {
public:
    ImmediateIntAwaiter(int value, int *ready_checks) noexcept : value_(value), ready_checks_(ready_checks) {}

    ImmediateIntAwaiter(const ImmediateIntAwaiter &) = delete;
    ImmediateIntAwaiter &operator=(const ImmediateIntAwaiter &) = delete;
    ImmediateIntAwaiter(ImmediateIntAwaiter &&) = delete;
    ImmediateIntAwaiter &operator=(ImmediateIntAwaiter &&) = delete;

    bool await_ready() noexcept {
        ++*ready_checks_;
        completed_ = true;
        return true;
    }

    bool await_suspend(std::coroutine_handle<>) noexcept {
        completed_ = true;
        return false;
    }

    int await_resume() const noexcept {
        FIBER_ASSERT(completed_);
        return value_;
    }

    [[nodiscard]] bool completed() const noexcept { return completed_; }

private:
    int value_ = 0;
    int *ready_checks_ = nullptr;
    bool completed_ = false;
};

class PendingAwaiter {
public:
    explicit PendingAwaiter(bool *canceled) noexcept : canceled_(canceled) {}

    PendingAwaiter(const PendingAwaiter &) = delete;
    PendingAwaiter &operator=(const PendingAwaiter &) = delete;
    PendingAwaiter(PendingAwaiter &&) = delete;
    PendingAwaiter &operator=(PendingAwaiter &&) = delete;

    ~PendingAwaiter() {
        if (armed_) {
            *canceled_ = true;
            handle_ = {};
        }
    }

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle) noexcept {
        handle_ = handle;
        armed_ = true;
        return true;
    }

    void await_resume() const noexcept { FIBER_ASSERT(completed_); }

    [[nodiscard]] bool completed() const noexcept { return completed_; }

private:
    bool *canceled_ = nullptr;
    std::coroutine_handle<> handle_{};
    bool armed_ = false;
    bool completed_ = false;
};

class SuspendFalseAwaiter {
public:
    explicit SuspendFalseAwaiter(int value) noexcept : value_(value) {}

    SuspendFalseAwaiter(const SuspendFalseAwaiter &) = delete;
    SuspendFalseAwaiter &operator=(const SuspendFalseAwaiter &) = delete;
    SuspendFalseAwaiter(SuspendFalseAwaiter &&) = delete;
    SuspendFalseAwaiter &operator=(SuspendFalseAwaiter &&) = delete;

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<>) noexcept {
        completed_ = true;
        return false;
    }

    int await_resume() const noexcept {
        FIBER_ASSERT(completed_);
        return value_;
    }

    [[nodiscard]] bool completed() const noexcept { return completed_; }

private:
    int value_ = 0;
    bool completed_ = false;
};

class StandardAwaiterOnly {
public:
    bool await_ready() const noexcept { return true; }
    void await_suspend(std::coroutine_handle<>) noexcept {}
    void await_resume() const noexcept {}
};

class MoveOnlyResult {
public:
    explicit MoveOnlyResult(int value) noexcept : value_(value) {}

    MoveOnlyResult(const MoveOnlyResult &) = delete;
    MoveOnlyResult &operator=(const MoveOnlyResult &) = delete;
    MoveOnlyResult(MoveOnlyResult &&) noexcept = default;
    MoveOnlyResult &operator=(MoveOnlyResult &&) noexcept = default;

    [[nodiscard]] int value() const noexcept { return value_; }

private:
    int value_ = 0;
};

class DestructionFlag {
public:
    explicit DestructionFlag(bool *destroyed) noexcept : destroyed_(destroyed) {}
    ~DestructionFlag() { *destroyed_ = true; }

private:
    bool *destroyed_ = nullptr;
};

fiber::async::Task<int> immediate_task(bool *started) {
    *started = true;
    co_return 51;
}

fiber::async::Task<MoveOnlyResult> delayed_move_only_task() {
    co_await fiber::async::sleep(std::chrono::milliseconds(1));
    co_return MoveOnlyResult(72);
}

fiber::async::Task<int> cancelable_task(bool *started, bool *frame_destroyed, bool *finished) {
    DestructionFlag flag(frame_destroyed);
    *started = true;
    co_await fiber::async::sleep(std::chrono::milliseconds(40));
    *finished = true;
    co_return 93;
}

fiber::async::Task<void> immediate_void_task(bool *started) {
    *started = true;
    co_return;
}

static_assert(fiber::async::SelectableAwaiter<ImmediateIntAwaiter>);
static_assert(fiber::async::SelectableAwaiter<PendingAwaiter>);
static_assert(fiber::async::SelectableAwaiter<SuspendFalseAwaiter>);
static_assert(fiber::async::SelectableAwaiter<fiber::async::SleepAwaiter>);
static_assert(fiber::async::SelectableAwaiter<fiber::async::YieldAwaiter>);
static_assert(fiber::async::SelectableAwaiter<fiber::async::WaitGroup::JoinAwaiter>);
static_assert(fiber::async::SelectableAwaiter<fiber::async::Mutex::LockAwaiter>);
static_assert(fiber::async::SelectableAwaiter<fiber::async::RWMutex::WriteLockAwaiter>);
static_assert(fiber::async::SelectableAwaiter<fiber::async::RWMutex::ReadLockAwaiter>);
static_assert(fiber::async::SelectableAwaiter<fiber::async::SignalAwaiter>);
static_assert(fiber::async::SelectableAwaiter<fiber::async::Watch<int>::Subscriber::NextAwaiter>);
using SleepTimeoutAwaiter = decltype(fiber::async::timeout_for(fiber::async::sleep(std::chrono::milliseconds(1)),
                                                               std::chrono::milliseconds(2)));
static_assert(fiber::async::SelectableAwaiter<SleepTimeoutAwaiter>);
using IntTaskSelectAwaiter = decltype(std::declval<fiber::async::Task<int> &&>().select());
using VoidTaskSelectAwaiter = decltype(std::declval<fiber::async::Task<void> &&>().select());
static_assert(fiber::async::SelectableAwaiter<IntTaskSelectAwaiter>);
static_assert(fiber::async::SelectableAwaiter<VoidTaskSelectAwaiter>);
static_assert(!std::is_move_constructible_v<IntTaskSelectAwaiter>);
static_assert(!fiber::async::SelectableAwaiter<StandardAwaiterOnly>);
static_assert(!fiber::async::SelectableAwaiter<fiber::async::Task<void>::Awaiter>);

struct WhenAnyOutcome {
    std::size_t immediate_index = 0;
    int immediate_value = 0;
    int first_ready_checks = 0;
    int second_ready_checks = 0;
    std::size_t suspend_index = 0;
    int suspend_value = 0;
    bool pending_canceled = false;
    bool saved_pending_canceled = false;
    int saved_value = 0;
    std::size_t sleep_index = 0;
    bool mutex_guard_owned = false;
    std::size_t nested_outer_index = 0;
    std::size_t nested_inner_index = 0;
    bool immediate_task_started = false;
    int immediate_task_value = 0;
    bool second_immediate_task_not_started = false;
    int move_only_task_value = 0;
    bool canceled_task_never_started = false;
    bool loser_task_started = false;
    bool loser_task_frame_destroyed = false;
    bool loser_task_finished = false;
    bool void_task_started = false;
};

DetachedTask exercise_when_any(std::promise<WhenAnyOutcome> *promise) {
    WhenAnyOutcome outcome;

    auto immediate =
            co_await fiber::async::when_any([&]() { return ImmediateIntAwaiter(11, &outcome.first_ready_checks); },
                                            [&]() { return ImmediateIntAwaiter(22, &outcome.second_ready_checks); });
    outcome.immediate_index = immediate.index();
    outcome.immediate_value = std::move(immediate).get<0>();

    bool pending_canceled = false;
    auto suspend_false = co_await fiber::async::when_any([&]() { return PendingAwaiter(&pending_canceled); },
                                                         []() { return SuspendFalseAwaiter(33); });
    outcome.suspend_index = suspend_false.index();
    outcome.suspend_value = std::move(suspend_false).get<1>();
    outcome.pending_canceled = pending_canceled;

    bool saved_pending_canceled = false;
    auto saved_combination = fiber::async::when_any([&]() { return PendingAwaiter(&saved_pending_canceled); },
                                                    []() { return SuspendFalseAwaiter(44); });
    auto saved_result = co_await saved_combination;
    outcome.saved_pending_canceled = saved_pending_canceled;
    outcome.saved_value = std::move(saved_result).get<1>();

    auto sleep_result =
            co_await fiber::async::when_any([]() { return fiber::async::sleep(std::chrono::milliseconds(1)); },
                                            []() { return fiber::async::sleep(std::chrono::milliseconds(40)); });
    outcome.sleep_index = sleep_result.index();
    sleep_result.get<0>();
    co_await fiber::async::sleep(std::chrono::milliseconds(50));

    fiber::async::Mutex mutex;
    auto mutex_result = co_await fiber::async::when_any(
            [&]() { return mutex.lock(); }, []() { return fiber::async::sleep(std::chrono::milliseconds(20)); });
    auto guard = std::move(mutex_result).get<0>();
    outcome.mutex_guard_owned = guard.owns_lock();
    guard.unlock();

    auto nested = co_await fiber::async::when_any(
            []() {
                return fiber::async::when_any([]() { return fiber::async::sleep(std::chrono::milliseconds(1)); },
                                              []() { return fiber::async::sleep(std::chrono::milliseconds(20)); });
            },
            []() { return fiber::async::sleep(std::chrono::milliseconds(40)); });
    outcome.nested_outer_index = nested.index();
    auto &&inner = std::move(nested).get<0>();
    outcome.nested_inner_index = inner.index();

    auto immediate_task_result =
            co_await fiber::async::when_any([&]() { return immediate_task(&outcome.immediate_task_started).select(); },
                                            []() { return fiber::async::sleep(std::chrono::milliseconds(20)); });
    outcome.immediate_task_value = std::move(immediate_task_result).get<0>();

    bool first_immediate_task_started = false;
    bool second_immediate_task_started = false;
    auto immediate_task_pair_result =
            co_await fiber::async::when_any([&]() { return immediate_task(&first_immediate_task_started).select(); },
                                            [&]() { return immediate_task(&second_immediate_task_started).select(); });
    std::move(immediate_task_pair_result).get<0>();
    outcome.second_immediate_task_not_started = first_immediate_task_started && !second_immediate_task_started;

    auto move_only_task_result =
            co_await fiber::async::when_any([]() { return delayed_move_only_task().select(); },
                                            []() { return fiber::async::sleep(std::chrono::milliseconds(30)); });
    auto move_only_value = std::move(move_only_task_result).get<0>();
    outcome.move_only_task_value = move_only_value.value();

    bool canceled_task_started = false;
    auto canceled_start_result =
            co_await fiber::async::when_any([&]() { return immediate_task(&canceled_task_started).select(); },
                                            []() { return SuspendFalseAwaiter(81); });
    outcome.canceled_task_never_started = !canceled_task_started;
    std::move(canceled_start_result).get<1>();

    auto loser_task_result = co_await fiber::async::when_any(
            [&]() {
                return cancelable_task(&outcome.loser_task_started, &outcome.loser_task_frame_destroyed,
                                       &outcome.loser_task_finished)
                        .select();
            },
            []() { return fiber::async::yield(); });
    loser_task_result.get<1>();
    co_await fiber::async::sleep(std::chrono::milliseconds(50));

    auto void_task_result =
            co_await fiber::async::when_any([&]() { return immediate_void_task(&outcome.void_task_started).select(); },
                                            []() { return fiber::async::sleep(std::chrono::milliseconds(20)); });
    void_task_result.get<0>();

    promise->set_value(outcome);
    fiber::event::EventLoop::current().stop();
    co_return;
}

} // namespace

TEST(WhenAnyTest, SupportsNonMovableAwaitersResultsCancellationAndNesting) {
    fiber::event::EventLoopGroup group(1);
    std::promise<WhenAnyOutcome> promise;
    auto future = promise.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() { return exercise_when_any(&promise); });

    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "when_any exercise did not finish in time";
        return;
    }

    WhenAnyOutcome outcome = future.get();
    EXPECT_EQ(outcome.immediate_index, 0U);
    EXPECT_EQ(outcome.immediate_value, 11);
    EXPECT_EQ(outcome.first_ready_checks, 1);
    EXPECT_EQ(outcome.second_ready_checks, 0);
    EXPECT_EQ(outcome.suspend_index, 1U);
    EXPECT_EQ(outcome.suspend_value, 33);
    EXPECT_TRUE(outcome.pending_canceled);
    EXPECT_TRUE(outcome.saved_pending_canceled);
    EXPECT_EQ(outcome.saved_value, 44);
    EXPECT_EQ(outcome.sleep_index, 0U);
    EXPECT_TRUE(outcome.mutex_guard_owned);
    EXPECT_EQ(outcome.nested_outer_index, 0U);
    EXPECT_EQ(outcome.nested_inner_index, 0U);
    EXPECT_TRUE(outcome.immediate_task_started);
    EXPECT_EQ(outcome.immediate_task_value, 51);
    EXPECT_TRUE(outcome.second_immediate_task_not_started);
    EXPECT_EQ(outcome.move_only_task_value, 72);
    EXPECT_TRUE(outcome.canceled_task_never_started);
    EXPECT_TRUE(outcome.loser_task_started);
    EXPECT_TRUE(outcome.loser_task_frame_destroyed);
    EXPECT_FALSE(outcome.loser_task_finished);
    EXPECT_TRUE(outcome.void_task_started);
    group.join();
}
