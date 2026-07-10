#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>

#include "async/Sleep.h"
#include "async/Spawn.h"
#include "async/Yield.h"
#include "event/EventLoop.h"
#include "event/EventLoopGroup.h"

using namespace std::chrono_literals;

TEST(SpawnTest, RetainsCoroutineLambdaCapturesUntilCompletion) {
    fiber::event::EventLoopGroup group(1);
    std::promise<void> resumed;
    auto resumed_future = resumed.get_future();
    std::promise<int> completed;
    auto completed_future = completed.get_future();

    auto retained = std::make_shared<int>(42);
    std::weak_ptr<int> weak = retained;

    group.start();
    fiber::async::spawn(group.at(0), [retained, &resumed, &completed]() -> fiber::async::DetachedTask {
        auto *resumed_ptr = &resumed;
        auto *completed_ptr = &completed;
        co_await fiber::async::yield();
        resumed_ptr->set_value();
        co_await fiber::async::sleep(20ms);
        completed_ptr->set_value(*retained);
        fiber::event::EventLoop::current().stop();
    });
    retained.reset();

    ASSERT_EQ(resumed_future.wait_for(2s), std::future_status::ready);
    EXPECT_FALSE(weak.expired());
    ASSERT_EQ(completed_future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(completed_future.get(), 42);

    group.join();
    EXPECT_TRUE(weak.expired());
}
