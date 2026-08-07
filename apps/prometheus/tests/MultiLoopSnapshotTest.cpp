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
#include "fiber/prometheus/MetricsRegistry.h"

namespace {

using fiber::async::DetachedTask;
using fiber::common::IoErr;
using fiber::common::IoResult;
using fiber::prometheus::CounterRef;
using fiber::prometheus::GaugeReduction;
using fiber::prometheus::GaugeRef;
using fiber::prometheus::HistogramRef;
using fiber::prometheus::HistogramUnit;
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

DetachedTask update_metrics(CounterRef counter, GaugeRef gauge, HistogramRef histogram, std::uint64_t counter_value,
                            std::int64_t gauge_value, std::uint64_t observation, std::promise<void> *done) {
    counter.add(counter_value);
    gauge.set(gauge_value);
    histogram.observe(observation);
    done->set_value();
    co_return;
}

DetachedTask collect_to_string(MetricsRegistry *registry, std::promise<IoResult<std::string>> *done,
                               fiber::event::EventLoopGroup *stop_group = nullptr) {
    auto &pool = fiber::event::EventLoop::current().io_buf_node_pool();
    auto result = co_await registry->collect_text(pool);
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
    auto &pool = fiber::event::EventLoop::current().io_buf_node_pool();
    (void) co_await registry->collect_text(pool);
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

TEST(MultiLoopSnapshotTest, CollectsOnOwnerLoopsAndAggregatesDeterministically) {
    fiber::event::EventLoopGroup loops(2);
    MetricsRegistry registry;

    auto counter_family = registry.register_counter("requests_total", "Requests");
    auto gauge_family = registry.register_gauge("connections", "Connections", GaugeReduction::Sum);
    auto histogram_family = registry.register_histogram(
            "request_duration_seconds", "Duration", std::array<std::uint64_t, 2>{1, 5}, HistogramUnit::Microseconds);
    ASSERT_TRUE(counter_family);
    ASSERT_TRUE(gauge_family);
    ASSERT_TRUE(histogram_family);
    auto counter_series = registry.register_series(*counter_family);
    auto gauge_series = registry.register_series(*gauge_family);
    auto histogram_series = registry.register_series(*histogram_family);
    auto shard0_id = registry.add_shard(loops.at(0));
    auto shard1_id = registry.add_shard(loops.at(1));
    ASSERT_TRUE(counter_series);
    ASSERT_TRUE(gauge_series);
    ASSERT_TRUE(histogram_series);
    ASSERT_TRUE(shard0_id);
    ASSERT_TRUE(shard1_id);
    ASSERT_TRUE(registry.freeze());

    auto counter0 = registry.shard(*shard0_id)->counter(*counter_series);
    auto gauge0 = registry.shard(*shard0_id)->gauge(*gauge_series);
    auto histogram0 = registry.shard(*shard0_id)->histogram(*histogram_series);
    auto counter1 = registry.shard(*shard1_id)->counter(*counter_series);
    auto gauge1 = registry.shard(*shard1_id)->gauge(*gauge_series);
    auto histogram1 = registry.shard(*shard1_id)->histogram(*histogram_series);
    ASSERT_TRUE(counter0);
    ASSERT_TRUE(gauge0);
    ASSERT_TRUE(histogram0);
    ASSERT_TRUE(counter1);
    ASSERT_TRUE(gauge1);
    ASSERT_TRUE(histogram1);

    std::promise<void> updated0;
    std::promise<void> updated1;
    auto updated0_future = updated0.get_future();
    auto updated1_future = updated1.get_future();
    std::promise<IoResult<std::string>> collected;
    auto collected_future = collected.get_future();

    loops.start();
    fiber::async::spawn(loops.at(0),
                        [&]() { return update_metrics(*counter0, *gauge0, *histogram0, 4, 2, 1, &updated0); });
    fiber::async::spawn(loops.at(1),
                        [&]() { return update_metrics(*counter1, *gauge1, *histogram1, 6, 3, 7, &updated1); });
    ASSERT_EQ(updated0_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ASSERT_EQ(updated1_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);

    fiber::async::spawn(loops.at(0), [&]() { return collect_to_string(&registry, &collected, &loops); });
    ASSERT_EQ(collected_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    auto result = collected_future.get();
    ASSERT_TRUE(result);
    EXPECT_EQ(*result, "# HELP requests_total Requests\n"
                       "# TYPE requests_total counter\n"
                       "requests_total 10\n"
                       "# HELP connections Connections\n"
                       "# TYPE connections gauge\n"
                       "connections 5\n"
                       "# HELP request_duration_seconds Duration\n"
                       "# TYPE request_duration_seconds histogram\n"
                       "request_duration_seconds_bucket{le=\"0.000001\"} 1\n"
                       "request_duration_seconds_bucket{le=\"0.000005\"} 1\n"
                       "request_duration_seconds_bucket{le=\"+Inf\"} 2\n"
                       "request_duration_seconds_sum 0.000008\n"
                       "request_duration_seconds_count 2\n");
    loops.join();
}

TEST(MultiLoopSnapshotTest, ConcurrentCollectReturnsBusyAndStopWaitsForInflight) {
    fiber::event::EventLoopGroup loops(2);
    MetricsRegistry registry;
    auto family = registry.register_counter("requests_total", "Requests");
    ASSERT_TRUE(family);
    ASSERT_TRUE(registry.register_series(*family));
    ASSERT_TRUE(registry.add_shard(loops.at(1)));
    ASSERT_TRUE(registry.freeze());

    std::atomic<bool> release{false};
    std::promise<void> blocker_entered;
    auto blocker_entered_future = blocker_entered.get_future();
    BlockingCallback blocker{.release = &release, .entered = &blocker_entered};
    std::promise<IoResult<std::string>> first;
    std::promise<IoResult<std::string>> second;
    std::promise<IoResult<std::string>> canceled;
    auto first_future = first.get_future();
    auto second_future = second.get_future();
    auto canceled_future = canceled.get_future();
    std::promise<void> idle;
    auto idle_future = idle.get_future();

    loops.start();
    loops.at(1).post<BlockingCallback, &BlockingCallback::notify_entry, &BlockingCallback::run>(blocker);
    ASSERT_EQ(blocker_entered_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);

    fiber::async::spawn(loops.at(0), [&]() { return collect_to_string(&registry, &first); });
    fiber::async::spawn(loops.at(0), [&]() { return collect_to_string(&registry, &second); });
    ASSERT_EQ(second_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    auto second_result = second_future.get();
    ASSERT_FALSE(second_result);
    EXPECT_EQ(second_result.error(), IoErr::Busy);

    registry.stop_collecting();
    fiber::async::spawn(loops.at(0), [&]() { return collect_to_string(&registry, &canceled); });
    fiber::async::spawn(loops.at(0), [&]() { return wait_idle_then_signal(&registry, &idle); });
    ASSERT_EQ(canceled_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    auto canceled_result = canceled_future.get();
    ASSERT_FALSE(canceled_result);
    EXPECT_EQ(canceled_result.error(), IoErr::Canceled);
    EXPECT_EQ(idle_future.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);

    release.store(true, std::memory_order_release);
    ASSERT_EQ(first_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_TRUE(first_future.get());
    ASSERT_EQ(idle_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    loops.stop();
    loops.join();
}

TEST(MultiLoopSnapshotTest, DestroyedCollectTaskLeavesCallbacksOwnedByRegistry) {
    fiber::event::EventLoopGroup loops(2);
    MetricsRegistry registry;
    auto family = registry.register_counter("requests_total", "Requests");
    ASSERT_TRUE(family);
    ASSERT_TRUE(registry.register_series(*family));
    ASSERT_TRUE(registry.add_shard(loops.at(1)));
    ASSERT_TRUE(registry.freeze());

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
    std::promise<void> idle;
    auto idle_future = idle.get_future();

    loops.start();
    loops.at(1).post<BlockingCallback, &BlockingCallback::notify_entry, &BlockingCallback::run>(blocker);
    ASSERT_EQ(blocker_entered_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    loops.at(0).post<StartCancelable, &StartCancelable::notify_entry, &StartCancelable::run>(start);
    ASSERT_EQ(started_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    loops.at(0).post<CancelCollect, &CancelCollect::notify_entry, &CancelCollect::run>(cancel);
    ASSERT_EQ(canceled_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);

    registry.stop_collecting();
    fiber::async::spawn(loops.at(0), [&]() { return wait_idle_then_signal(&registry, &idle); });
    EXPECT_EQ(idle_future.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);
    release.store(true, std::memory_order_release);
    ASSERT_EQ(idle_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_FALSE(collect_completed);

    loops.stop();
    loops.join();
}

} // namespace
