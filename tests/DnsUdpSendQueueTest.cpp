#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <future>

#include "async/Sleep.h"
#include "async/Spawn.h"
#include "dns/detail/DnsUdpSendQueue.h"
#include "event/EventLoopGroup.h"

namespace {

using namespace std::chrono_literals;
using fiber::async::DetachedTask;
using fiber::common::IoErr;
using fiber::dns::detail::DnsUdpSendQueue;

struct QueueOutcome {
    IoErr err = IoErr::None;
    std::array<int, 3> order{};
    std::size_t order_size = 0;
    std::size_t canceled = 0;
    bool entered_before_release = false;
    bool idle = false;
    bool reusable = false;
};

DetachedTask run_fifo_queue_test(fiber::event::EventLoop *loop, std::promise<QueueOutcome> *promise) {
    QueueOutcome outcome;
    DnsUdpSendQueue queue;
    queue.init(*loop);
    auto first_owner = queue.take_ownership_after_would_block();

    std::size_t completed = 0;
    auto run_waiter = [&](int id) -> DetachedTask {
        auto owner_result = co_await queue.acquire();
        if (!owner_result) {
            outcome.err = owner_result.error();
            ++completed;
            co_return;
        }

        auto owner = std::move(*owner_result);
        outcome.order[outcome.order_size++] = id;
        owner.release();
        ++completed;
        co_return;
    };

    fiber::async::spawn(*loop, [&]() { return run_waiter(1); });
    fiber::async::spawn(*loop, [&]() { return run_waiter(2); });
    fiber::async::spawn(*loop, [&]() { return run_waiter(3); });

    co_await fiber::async::sleep(5ms);
    outcome.entered_before_release = outcome.order_size != 0;
    first_owner.release();
    while (completed != 3) {
        co_await fiber::async::sleep(1ms);
    }

    outcome.idle = queue.idle();
    queue.close();
    queue.reset();
    promise->set_value(outcome);
}

DetachedTask run_close_queue_test(fiber::event::EventLoop *loop, std::promise<QueueOutcome> *promise) {
    QueueOutcome outcome;
    DnsUdpSendQueue queue;
    queue.init(*loop);
    auto first_owner = queue.take_ownership_after_would_block();

    std::size_t completed = 0;
    auto run_waiter = [&]() -> DetachedTask {
        auto owner_result = co_await queue.acquire();
        if (!owner_result && owner_result.error() == IoErr::Canceled) {
            ++outcome.canceled;
        } else if (!owner_result) {
            outcome.err = owner_result.error();
        } else {
            outcome.err = IoErr::Invalid;
            auto owner = std::move(*owner_result);
            owner.release();
        }
        ++completed;
        co_return;
    };

    fiber::async::spawn(*loop, [&]() { return run_waiter(); });
    fiber::async::spawn(*loop, [&]() { return run_waiter(); });
    co_await fiber::async::sleep(5ms);

    queue.close();
    first_owner.release();
    while (completed != 2) {
        co_await fiber::async::sleep(1ms);
    }

    auto closed_result = co_await queue.acquire();
    if (closed_result || closed_result.error() != IoErr::Canceled) {
        outcome.err = IoErr::Invalid;
    }
    outcome.idle = queue.idle();

    queue.reset();
    queue.init(*loop);
    outcome.reusable = queue.fast_path_available();
    auto reused_owner = queue.take_ownership_after_would_block();
    reused_owner.release();
    outcome.reusable = outcome.reusable && queue.idle();
    queue.close();
    queue.reset();
    promise->set_value(outcome);
}

} // namespace

TEST(DnsUdpSendQueueTest, SerializesSlowSendersInFifoOrder) {
    fiber::event::EventLoopGroup group(1);
    std::promise<QueueOutcome> promise;
    auto future = promise.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() { return run_fifo_queue_test(&group.at(0), &promise); });

    if (future.wait_for(2s) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "FIFO send queue test did not complete in time";
        return;
    }
    const QueueOutcome outcome = future.get();
    group.stop();
    group.join();

    EXPECT_EQ(outcome.err, IoErr::None);
    EXPECT_FALSE(outcome.entered_before_release);
    ASSERT_EQ(outcome.order_size, 3u);
    EXPECT_EQ(outcome.order[0], 1);
    EXPECT_EQ(outcome.order[1], 2);
    EXPECT_EQ(outcome.order[2], 3);
    EXPECT_TRUE(outcome.idle);
}

TEST(DnsUdpSendQueueTest, CloseCancelsQueuedSendersAndAllowsReset) {
    fiber::event::EventLoopGroup group(1);
    std::promise<QueueOutcome> promise;
    auto future = promise.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() { return run_close_queue_test(&group.at(0), &promise); });

    if (future.wait_for(2s) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "send queue close test did not complete in time";
        return;
    }
    const QueueOutcome outcome = future.get();
    group.stop();
    group.join();

    EXPECT_EQ(outcome.err, IoErr::None);
    EXPECT_EQ(outcome.canceled, 2u);
    EXPECT_TRUE(outcome.idle);
    EXPECT_TRUE(outcome.reusable);
}
