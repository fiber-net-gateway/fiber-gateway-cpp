#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <exception>
#include <future>
#include <optional>
#include <thread>
#include <utility>

#include "async/CoroutinePromiseBase.h"
#include "async/Spawn.h"
#include "async/Timeout.h"
#include "async/Watch.h"
#include "event/EventLoopGroup.h"

namespace {

using DetachedTask = fiber::async::DetachedTask;
using IntWatch = fiber::async::Watch<int>;
using IntSnapshot = IntWatch::Snapshot;

class ManualTask {
public:
    struct promise_type : fiber::async::CoroutinePromiseBase {
        ManualTask get_return_object() noexcept { return ManualTask(handle_type::from_promise(*this)); }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() { std::terminate(); }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    explicit ManualTask(handle_type handle) noexcept : handle_(handle) {}
    ManualTask(const ManualTask &) = delete;
    ManualTask &operator=(const ManualTask &) = delete;
    ManualTask(ManualTask &&other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

    ~ManualTask() {
        if (handle_) {
            handle_.destroy();
        }
    }

    handle_type release() noexcept { return std::exchange(handle_, nullptr); }

private:
    handle_type handle_{};
};

DetachedTask await_next_and_stop(IntWatch::Subscriber *subscriber, std::uint64_t received_version,
                                 std::promise<IntSnapshot> *result) {
    auto snapshot = co_await subscriber->next(received_version);
    result->set_value(std::move(snapshot));
    fiber::event::EventLoop::current().stop();
    co_return;
}

DetachedTask publish_two_values(IntWatch::Publisher *publisher) {
    publisher->publish(10);
    publisher->publish(11);
    co_return;
}

DetachedTask suspend_next_then_publish(IntWatch::Subscriber *subscriber, IntWatch::Publisher *publisher,
                                       std::promise<IntSnapshot> *result) {
    fiber::async::spawn([publisher]() { return publish_two_values(publisher); });
    auto snapshot = co_await subscriber->next(0);
    result->set_value(std::move(snapshot));
    fiber::event::EventLoop::current().stop();
    co_return;
}

DetachedTask await_next_and_record_thread(IntWatch::Subscriber *subscriber, std::promise<IntSnapshot> *value_result,
                                          std::promise<std::thread::id> *thread_result) {
    auto snapshot = co_await subscriber->next(0);
    value_result->set_value(std::move(snapshot));
    thread_result->set_value(std::this_thread::get_id());
    co_return;
}

DetachedTask record_thread(std::promise<std::thread::id> *result) {
    result->set_value(std::this_thread::get_id());
    co_return;
}

ManualTask await_next_then_increment(IntWatch::Subscriber *subscriber, std::atomic<int> *hits) {
    co_await subscriber->next(0);
    hits->fetch_add(1, std::memory_order_relaxed);
    co_return;
}

DetachedTask cancel_pending_next(IntWatch::Subscriber *subscriber, IntWatch::Publisher *publisher,
                                 std::atomic<int> *hits, std::promise<void> *done) {
    auto task = await_next_then_increment(subscriber, hits);
    auto handle = task.release();
    handle.resume();
    handle.destroy();

    publisher->publish(1);
    done->set_value();
    fiber::event::EventLoop::current().stop();
    co_return;
}

DetachedTask timeout_then_observe_next(IntWatch::Subscriber *subscriber, IntWatch::Publisher *publisher,
                                       std::promise<int> *result) {
    auto timeout_result = co_await fiber::async::timeout_for([subscriber]() { return subscriber->next(0); },
                                                             std::chrono::milliseconds(1));
    if (timeout_result || timeout_result.error() != fiber::common::IoErr::TimedOut) {
        result->set_value(-1);
        fiber::event::EventLoop::current().stop();
        co_return;
    }

    publisher->publish(12);
    auto snapshot = co_await subscriber->next(0);
    result->set_value(snapshot.value ? *snapshot.value : -1);
    fiber::event::EventLoop::current().stop();
    co_return;
}

} // namespace

TEST(WatchTest, EmptyAndInitialCurrentValues) {
    IntWatch empty_watch;
    auto empty_subscriber = empty_watch.subscribe();
    auto empty = empty_subscriber.current();
    EXPECT_EQ(empty.value, nullptr);
    EXPECT_EQ(empty.version, 0u);

    IntWatch initialized_watch(42);
    auto initialized_subscriber = initialized_watch.subscribe();
    auto initialized = initialized_subscriber.current();
    ASSERT_NE(initialized.value, nullptr);
    EXPECT_EQ(*initialized.value, 42);
    EXPECT_EQ(initialized.version, 1u);
}

TEST(WatchTest, PublisherCanOnlyBeAcquiredOnce) {
    IntWatch watch;

    auto first = watch.acquire_publisher();
    ASSERT_TRUE(first.has_value());
    first.reset();

    auto second = watch.acquire_publisher();
    EXPECT_FALSE(second.has_value());
}

TEST(WatchTest, HandlesOutliveWatchObject) {
    std::optional<IntWatch::Subscriber> subscriber;

    {
        IntWatch watch;
        auto publisher = watch.acquire_publisher();
        ASSERT_TRUE(publisher.has_value());
        subscriber.emplace(watch.subscribe());
        publisher->publish(7);
    }

    auto snapshot = subscriber->current();
    ASSERT_NE(snapshot.value, nullptr);
    EXPECT_EQ(*snapshot.value, 7);
    EXPECT_EQ(snapshot.version, 1u);
}

TEST(WatchTest, NextZeroReturnsExistingInitialSnapshot) {
    IntWatch watch(42);
    auto subscriber = watch.subscribe();

    fiber::event::EventLoopGroup group(1);
    std::promise<IntSnapshot> result;
    auto future = result.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&subscriber, &result]() { return await_next_and_stop(&subscriber, 0, &result); });

    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "next(0) did not return the initial snapshot in time";
        return;
    }

    IntSnapshot snapshot = future.get();
    ASSERT_NE(snapshot.value, nullptr);
    EXPECT_EQ(*snapshot.value, 42);
    EXPECT_EQ(snapshot.version, 1u);
    group.join();
}

TEST(WatchTest, NextReturnsLatestCoalescedValue) {
    IntWatch watch;
    auto publisher = watch.acquire_publisher();
    ASSERT_TRUE(publisher.has_value());
    auto subscriber = watch.subscribe();

    publisher->publish(1);
    publisher->publish(2);
    publisher->publish(3);

    fiber::event::EventLoopGroup group(1);
    std::promise<IntSnapshot> result;
    auto future = result.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&subscriber, &result]() { return await_next_and_stop(&subscriber, 0, &result); });

    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "next did not return the coalesced value in time";
        return;
    }

    IntSnapshot snapshot = future.get();
    ASSERT_NE(snapshot.value, nullptr);
    EXPECT_EQ(*snapshot.value, 3);
    EXPECT_EQ(snapshot.version, 3u);
    group.join();
}

TEST(WatchTest, PendingNextResumesAfterPublish) {
    IntWatch watch;
    auto publisher = watch.acquire_publisher();
    ASSERT_TRUE(publisher.has_value());
    auto subscriber = watch.subscribe();

    fiber::event::EventLoopGroup group(1);
    std::promise<IntSnapshot> result;
    auto future = result.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() { return suspend_next_then_publish(&subscriber, &*publisher, &result); });

    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "pending next was not resumed after publish";
        return;
    }

    IntSnapshot snapshot = future.get();
    ASSERT_NE(snapshot.value, nullptr);
    EXPECT_EQ(*snapshot.value, 11);
    EXPECT_EQ(snapshot.version, 2u);
    group.join();
}

TEST(WatchTest, CurrentDoesNotAffectExplicitNextVersion) {
    IntWatch watch;
    auto publisher = watch.acquire_publisher();
    ASSERT_TRUE(publisher.has_value());
    auto subscriber_a = watch.subscribe();
    auto subscriber_b = watch.subscribe();

    publisher->publish(1);
    auto snapshot_a = subscriber_a.current();
    ASSERT_NE(snapshot_a.value, nullptr);
    EXPECT_EQ(*snapshot_a.value, 1);
    EXPECT_EQ(snapshot_a.version, 1u);

    publisher->publish(2);
    auto snapshot_b = subscriber_b.current();
    ASSERT_NE(snapshot_b.value, nullptr);
    EXPECT_EQ(*snapshot_b.value, 2);
    EXPECT_EQ(snapshot_b.version, 2u);

    fiber::event::EventLoopGroup group(1);
    std::promise<IntSnapshot> result;
    auto future = result.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&subscriber_a, &result, version = snapshot_a.version]() {
        return await_next_and_stop(&subscriber_a, version, &result);
    });

    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "subscriber did not observe its independent next version";
        return;
    }

    IntSnapshot next = future.get();
    ASSERT_NE(next.value, nullptr);
    EXPECT_EQ(*next.value, 2);
    EXPECT_EQ(next.version, 2u);
    group.join();
}

TEST(WatchTest, PendingWaitersFromSameSubscriberResumeOnOwningLoops) {
    IntWatch watch;
    auto publisher = watch.acquire_publisher();
    ASSERT_TRUE(publisher.has_value());
    auto subscriber = watch.subscribe();

    fiber::event::EventLoopGroup group(2);
    std::promise<std::thread::id> loop_a_id;
    std::promise<std::thread::id> loop_b_id;
    auto loop_a_future = loop_a_id.get_future();
    auto loop_b_future = loop_b_id.get_future();

    std::promise<IntSnapshot> value_a;
    std::promise<IntSnapshot> value_b;
    auto value_a_future = value_a.get_future();
    auto value_b_future = value_b.get_future();
    std::promise<std::thread::id> resumed_a_id;
    std::promise<std::thread::id> resumed_b_id;
    auto resumed_a_future = resumed_a_id.get_future();
    auto resumed_b_future = resumed_b_id.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&loop_a_id]() { return record_thread(&loop_a_id); });
    fiber::async::spawn(group.at(1), [&loop_b_id]() { return record_thread(&loop_b_id); });

    if (loop_a_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready ||
        loop_b_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "failed to capture event loop thread ids";
        return;
    }

    const std::thread::id expected_a = loop_a_future.get();
    const std::thread::id expected_b = loop_b_future.get();

    fiber::async::spawn(group.at(0),
                        [&]() { return await_next_and_record_thread(&subscriber, &value_a, &resumed_a_id); });
    fiber::async::spawn(group.at(1),
                        [&]() { return await_next_and_record_thread(&subscriber, &value_b, &resumed_b_id); });

    publisher->publish(9);

    const bool completed = value_a_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready &&
                           value_b_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready &&
                           resumed_a_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready &&
                           resumed_b_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    group.stop();
    group.join();

    ASSERT_TRUE(completed);
    IntSnapshot snapshot_a = value_a_future.get();
    IntSnapshot snapshot_b = value_b_future.get();
    ASSERT_NE(snapshot_a.value, nullptr);
    ASSERT_NE(snapshot_b.value, nullptr);
    EXPECT_EQ(*snapshot_a.value, 9);
    EXPECT_EQ(*snapshot_b.value, 9);
    EXPECT_EQ(snapshot_a.version, 1u);
    EXPECT_EQ(snapshot_b.version, 1u);
    EXPECT_EQ(resumed_a_future.get(), expected_a);
    EXPECT_EQ(resumed_b_future.get(), expected_b);
}

TEST(WatchTest, DestroyingPendingCoroutineCancelsWaiter) {
    IntWatch watch;
    auto publisher = watch.acquire_publisher();
    ASSERT_TRUE(publisher.has_value());
    auto subscriber = watch.subscribe();
    std::atomic<int> hits{0};

    fiber::event::EventLoopGroup group(1);
    std::promise<void> done;
    auto future = done.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() { return cancel_pending_next(&subscriber, &*publisher, &hits, &done); });

    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "pending coroutine cancellation did not finish in time";
        return;
    }

    future.get();
    group.join();
    EXPECT_EQ(hits.load(std::memory_order_relaxed), 0);
}

TEST(WatchTest, SubscriberCanBeReusedAfterTimeout) {
    IntWatch watch;
    auto publisher = watch.acquire_publisher();
    ASSERT_TRUE(publisher.has_value());
    auto subscriber = watch.subscribe();

    fiber::event::EventLoopGroup group(1);
    std::promise<int> result;
    auto future = result.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() { return timeout_then_observe_next(&subscriber, &*publisher, &result); });

    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "subscriber timeout handling did not finish in time";
        return;
    }

    EXPECT_EQ(future.get(), 12);
    group.join();
}
