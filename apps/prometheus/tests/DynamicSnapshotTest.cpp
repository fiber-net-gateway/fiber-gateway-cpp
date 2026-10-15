#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <future>
#include <string>
#include <sys/uio.h>
#include <thread>

#include <fiber/async/Spawn.h>
#include <fiber/event/EventLoopGroup.h>
#include "DynamicSeriesStore.h"
#include "fiber/prometheus/MetricsRegistry.h"

namespace {

using fiber::async::DetachedTask;
using fiber::common::IoErr;
using fiber::common::IoResult;
using fiber::prometheus::CollectOptions;
using fiber::prometheus::MetricsRegistry;

std::string chain_string(const fiber::mem::IoBufChain &chain) {
    std::array<iovec, 64> iov{};
    const int count = chain.fill_write_iov(iov.data(), static_cast<int>(iov.size()));
    std::string result;
    for (int index = 0; index < count; ++index) {
        result.append(static_cast<const char *>(iov[index].iov_base), iov[index].iov_len);
    }
    return result;
}

DetachedTask collect_to_string(MetricsRegistry *registry, std::promise<IoResult<std::string>> *done,
                               fiber::event::EventLoopGroup *stop_group = nullptr) {
    auto result = co_await registry->collect_text();
    if (!result) {
        done->set_value(std::unexpected(result.error()));
    } else {
        done->set_value(chain_string(*result));
    }
    if (stop_group) {
        stop_group->stop();
    }
    co_return;
}

DetachedTask collect_into_string(MetricsRegistry *registry, std::promise<IoResult<std::string>> *done,
                                 CollectOptions options = {}) {
    fiber::mem::IoBuf out = fiber::mem::IoBuf::allocate(4096);
    auto result = co_await registry->collect_text_into(out, options);
    if (!result) {
        done->set_value(std::unexpected(result.error()));
    } else {
        done->set_value(std::string(reinterpret_cast<const char *>(out.readable_data()), out.readable()));
    }
    co_return;
}

DetachedTask signal_ready(std::promise<void> *done) {
    done->set_value();
    co_return;
}

struct BlockingCallback {
    std::atomic<bool> *release = nullptr;
    std::promise<void> *entered = nullptr;
    fiber::event::EventLoop::NotifyEntry notify_entry{};

    static void run(BlockingCallback *callback) noexcept {
        callback->entered->set_value();
        while (!callback->release->load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
};

DetachedTask wait_idle_then_signal(MetricsRegistry *registry, std::promise<void> *done) {
    co_await registry->wait_for_idle();
    done->set_value();
    co_return;
}

class CancelableTask {
public:
    struct promise_type;
    using Handle = std::coroutine_handle<promise_type>;

    struct promise_type {
        CancelableTask get_return_object() noexcept { return CancelableTask(Handle::from_promise(*this)); }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { std::terminate(); }
    };

    CancelableTask() noexcept = default;
    explicit CancelableTask(Handle handle) noexcept : handle_(handle) {}
    CancelableTask(const CancelableTask &) = delete;
    CancelableTask &operator=(const CancelableTask &) = delete;
    CancelableTask(CancelableTask &&other) noexcept : handle_(std::exchange(other.handle_, {})) {}
    CancelableTask &operator=(CancelableTask &&other) noexcept {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }
    ~CancelableTask() { reset(); }

    void start() noexcept { handle_.resume(); }
    void reset() noexcept {
        if (handle_) {
            handle_.destroy();
            handle_ = {};
        }
    }

private:
    Handle handle_{};
};

CancelableTask cancelable_collect(MetricsRegistry *registry, bool *completed) {
    (void) co_await registry->collect_text();
    *completed = true;
}

struct StartCancelable {
    MetricsRegistry *registry = nullptr;
    CancelableTask *task = nullptr;
    bool *completed = nullptr;
    std::promise<void> *started = nullptr;
    fiber::event::EventLoop::NotifyEntry notify_entry{};

    static void run(StartCancelable *request) noexcept {
        *request->task = cancelable_collect(request->registry, request->completed);
        request->task->start();
        request->started->set_value();
    }
};

struct CancelCollect {
    CancelableTask *task = nullptr;
    std::promise<void> *canceled = nullptr;
    fiber::event::EventLoop::NotifyEntry notify_entry{};

    static void run(CancelCollect *request) noexcept {
        request->task->reset();
        request->canceled->set_value();
    }
};

constexpr std::array<std::string_view, 1> kUser{"user"};
constexpr fiber::prometheus::SeriesOptions kLru{.mode = fiber::prometheus::SeriesMode::Lru, .max_series = 1};
std::array<std::string_view, 1> label(std::string_view value) { return {value}; }

TEST(DynamicSnapshotTest, SharedGenerationOwnsEvictedLabelsAndNextGenerationRefreshes) {
    fiber::event::EventLoopGroup loops(3);
    MetricsRegistry registry;
    auto dynamic_family = registry.register_counter("users_total", "Users", kUser, kLru);
    auto fixed_family = registry.register_counter("fixed_total", "Fixed");
    ASSERT_TRUE(dynamic_family);
    ASSERT_TRUE(fixed_family);
    auto series = registry.register_series(*fixed_family);
    auto shard = registry.add_shard(loops.at(1));
    ASSERT_TRUE(registry.add_shard(loops.at(0)));
    ASSERT_TRUE(series);
    ASSERT_TRUE(shard);
    ASSERT_TRUE(registry.freeze());
    auto counter = *registry.dynamic_counter(*dynamic_family);
    ASSERT_TRUE(counter.add(label("alice"), 7));
    std::atomic<bool> release{false};
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    BlockingCallback blocker{.release = &release, .entered = &entered};
    std::promise<IoResult<std::string>> first, second, small, refreshed;
    auto a = first.get_future();
    auto b = second.get_future();
    auto c = small.get_future();
    auto d = refreshed.get_future();
    std::promise<void> first_started, followers_started;
    auto started_a = first_started.get_future();
    auto started_b = followers_started.get_future();
    loops.start();
    loops.at(1).post<BlockingCallback, &BlockingCallback::notify_entry, &BlockingCallback::run>(blocker);
    ASSERT_EQ(entered_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    fiber::async::spawn(loops.at(0), [&]() { return collect_to_string(&registry, &first); });
    fiber::async::spawn(loops.at(0), [&]() { return signal_ready(&first_started); });
    ASSERT_EQ(started_a.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ASSERT_TRUE(counter.add(label("bob"), 9)); // Frees live alice before any encoder can run.
    fiber::async::spawn(loops.at(2), [&]() { return collect_into_string(&registry, &second); });
    fiber::async::spawn(loops.at(2), [&]() {
        return collect_into_string(&registry, &small, CollectOptions{.max_output_bytes = 1});
    });
    fiber::async::spawn(loops.at(2), [&]() { return signal_ready(&followers_started); });
    ASSERT_EQ(started_b.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(a.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);
    release.store(true, std::memory_order_release);
    ASSERT_EQ(a.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ASSERT_EQ(b.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ASSERT_EQ(c.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    auto result_a = a.get();
    auto result_b = b.get();
    auto result_c = c.get();
    ASSERT_TRUE(result_a);
    ASSERT_TRUE(result_b);
    ASSERT_FALSE(result_c);
    EXPECT_EQ(*result_a, *result_b);
    EXPECT_EQ(result_c.error(), IoErr::MessageTooLarge);
    EXPECT_EQ(*result_a, "# HELP users_total Users\n# TYPE users_total counter\nusers_total{user=\"alice\"} 7\n"
                         "# HELP fixed_total Fixed\n# TYPE fixed_total counter\nfixed_total 0\n");
    fiber::async::spawn(loops.at(0), [&]() { return collect_to_string(&registry, &refreshed); });
    ASSERT_EQ(d.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    auto next = d.get();
    ASSERT_TRUE(next);
    EXPECT_NE(next->find("user=\"bob\"} 9\n"), std::string::npos);
    EXPECT_EQ(next->find("alice"), std::string::npos);
    loops.stop();
    loops.join();
}

// The failure is set on the leader loop, without affecting the shard owner.
struct FailCapture {
    MetricsRegistry *registry;
    std::promise<IoResult<std::string>> *done;
    fiber::event::EventLoop::NotifyEntry notify_entry{};
    static void run(FailCapture *self) noexcept {
        fiber::prometheus::detail::fail_dynamic_allocation_after(0);
        fiber::async::spawn(fiber::event::EventLoop::current(),
                            [self]() { return collect_to_string(self->registry, self->done); });
    }
};

void shared_snapshot_failure(bool allocation_failure) {
    fiber::event::EventLoopGroup loops(2);
    MetricsRegistry registry({.max_dynamic_snapshot_bytes = allocation_failure ? 65536U : 200U});
    auto family = registry.register_counter("users_total", "Users", kUser, kLru);
    ASSERT_TRUE(family);
    ASSERT_TRUE(registry.add_shard(loops.at(1)));
    ASSERT_TRUE(registry.freeze());
    ASSERT_TRUE(registry.dynamic_counter(*family)->inc(label(std::string(150, 'x'))));
    std::atomic<bool> release{false};
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    BlockingCallback blocker{.release = &release, .entered = &entered};
    std::promise<IoResult<std::string>> first, second, canceled;
    auto a = first.get_future();
    auto b = second.get_future();
    auto c = canceled.get_future();
    std::promise<void> started, idle;
    auto started_future = started.get_future();
    auto idle_future = idle.get_future();
    FailCapture request{.registry = &registry, .done = &first};
    loops.start();
    loops.at(1).post<BlockingCallback, &BlockingCallback::notify_entry, &BlockingCallback::run>(blocker);
    ASSERT_EQ(entered_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    if (allocation_failure) {
        loops.at(0).post<FailCapture, &FailCapture::notify_entry, &FailCapture::run>(request);
    } else {
        fiber::async::spawn(loops.at(0), [&]() { return collect_to_string(&registry, &first); });
    }
    std::promise<void> leader_started;
    auto leader_future = leader_started.get_future();
    fiber::async::spawn(loops.at(0), [&]() { return signal_ready(&leader_started); });
    ASSERT_EQ(leader_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    fiber::async::spawn(loops.at(0), [&]() { return collect_into_string(&registry, &second); });
    fiber::async::spawn(loops.at(0), [&]() { return signal_ready(&started); });
    ASSERT_EQ(started_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    registry.stop_collecting();
    fiber::async::spawn(loops.at(0), [&]() { return wait_idle_then_signal(&registry, &idle); });
    fiber::async::spawn(loops.at(0), [&]() { return collect_to_string(&registry, &canceled); });
    EXPECT_EQ(a.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);
    EXPECT_EQ(b.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);
    EXPECT_EQ(idle_future.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);
    ASSERT_EQ(c.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(c.get().error(), IoErr::Canceled);
    release.store(true, std::memory_order_release);
    ASSERT_EQ(a.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ASSERT_EQ(b.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(a.get().error(), allocation_failure ? IoErr::NoMem : IoErr::MessageTooLarge);
    EXPECT_EQ(b.get().error(), allocation_failure ? IoErr::NoMem : IoErr::MessageTooLarge);
    ASSERT_EQ(idle_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    // stop_collecting stops reads only.
    EXPECT_TRUE(registry.dynamic_counter(*family)->inc(label("still-recording")));
    loops.stop();
    loops.join();
}

TEST(DynamicSnapshotTest, SnapshotBudgetFailureIsSharedAndWaitsForStaticCallbacks) { shared_snapshot_failure(false); }
TEST(DynamicSnapshotTest, SnapshotAllocationFailureIsSharedAndWaitsForStaticCallbacks) {
    shared_snapshot_failure(true);
}

TEST(DynamicSnapshotTest, AllocationFailureCanRecoverOnNextGenerationWithoutShards) {
    fiber::event::EventLoopGroup loops(1);
    MetricsRegistry registry;
    auto family = registry.register_counter("users_total", "Users", kUser, kLru);
    ASSERT_TRUE(family);
    ASSERT_TRUE(registry.freeze());
    ASSERT_TRUE(registry.dynamic_counter(*family)->inc(label("alice")));
    std::promise<IoResult<std::string>> first, second;
    auto a = first.get_future();
    auto b = second.get_future();
    FailCapture request{.registry = &registry, .done = &first};
    loops.start();
    loops.at(0).post<FailCapture, &FailCapture::notify_entry, &FailCapture::run>(request);
    ASSERT_EQ(a.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(a.get().error(), IoErr::NoMem);
    // Clear the thread-local injector on its own loop before retrying.
    fiber::async::spawn(loops.at(0), [&]() -> DetachedTask {
        fiber::prometheus::detail::fail_dynamic_allocation_after(-1);
        co_await registry.wait_for_idle();
        auto result = co_await registry.collect_text();
        if (result) {
            second.set_value(chain_string(*result));
        } else {
            second.set_value(std::unexpected(result.error()));
        }
    });
    ASSERT_EQ(b.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    auto result = b.get();
    ASSERT_TRUE(result);
    EXPECT_NE(result->find("user=\"alice\"} 1\n"), std::string::npos);
    loops.stop();
    loops.join();
}
void canceled_collector(bool cancel_leader) {
    fiber::event::EventLoopGroup loops(2);
    MetricsRegistry registry;
    auto family = registry.register_counter("requests_total", "Requests", kUser, kLru);
    ASSERT_TRUE(family);
    ASSERT_TRUE(registry.add_shard(loops.at(1)));
    ASSERT_TRUE(registry.freeze());
    auto counter = *registry.dynamic_counter(*family);
    ASSERT_TRUE(counter.add(label("alice"), 3));

    std::atomic<bool> release{false};
    std::promise<void> blocker_entered;
    auto blocker_entered_future = blocker_entered.get_future();
    BlockingCallback blocker{.release = &release, .entered = &blocker_entered};
    CancelableTask collect_task;
    bool collect_completed = false;
    std::promise<void> started;
    auto started_future = started.get_future();
    StartCancelable start{
            .registry = &registry, .task = &collect_task, .completed = &collect_completed, .started = &started};
    std::promise<void> canceled;
    auto canceled_future = canceled.get_future();
    CancelCollect cancel{.task = &collect_task, .canceled = &canceled};
    std::promise<IoResult<std::string>> follower;
    auto follower_future = follower.get_future();
    std::promise<void> follower_started;
    auto follower_started_future = follower_started.get_future();
    std::promise<void> idle;
    auto idle_future = idle.get_future();

    loops.start();
    loops.at(1).post<BlockingCallback, &BlockingCallback::notify_entry, &BlockingCallback::run>(blocker);
    ASSERT_EQ(blocker_entered_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    if (cancel_leader) {
        loops.at(0).post<StartCancelable, &StartCancelable::notify_entry, &StartCancelable::run>(start);
        ASSERT_EQ(started_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    }
    fiber::async::spawn(loops.at(0), [&]() { return collect_to_string(&registry, &follower); });
    fiber::async::spawn(loops.at(0), [&]() { return signal_ready(&follower_started); });
    ASSERT_EQ(follower_started_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    if (!cancel_leader) {
        loops.at(0).post<StartCancelable, &StartCancelable::notify_entry, &StartCancelable::run>(start);
        ASSERT_EQ(started_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    }
    loops.at(0).post<CancelCollect, &CancelCollect::notify_entry, &CancelCollect::run>(cancel);
    ASSERT_EQ(canceled_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);

    ASSERT_TRUE(counter.add(label("bob"), 7));
    registry.stop_collecting();
    fiber::async::spawn(loops.at(0), [&]() { return wait_idle_then_signal(&registry, &idle); });
    EXPECT_EQ(idle_future.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);
    EXPECT_EQ(follower_future.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);
    release.store(true, std::memory_order_release);
    ASSERT_EQ(follower_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    auto result = follower_future.get();
    ASSERT_TRUE(result);
    EXPECT_NE(result->find("user=\"alice\"} 3\n"), std::string::npos);
    EXPECT_EQ(result->find("bob"), std::string::npos);
    ASSERT_EQ(idle_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_FALSE(collect_completed);

    loops.stop();
    loops.join();
}
TEST(DynamicSnapshotTest, DestroyedLeaderLeavesOwnedDynamicSnapshotForFollower) { canceled_collector(true); }
TEST(DynamicSnapshotTest, DestroyedFollowerLeavesOwnedDynamicSnapshotForLeader) { canceled_collector(false); }

} // namespace
