#include <gtest/gtest.h>

#include <chrono>
#include <future>

#include "async/CoroutineFramePool.h"
#include "event/EventLoopGroup.h"

TEST(EventLoopTest, FramePoolInstalledOnLoopThread) {
    fiber::event::EventLoopGroup group(1);
    std::promise<bool> promise;
    auto future = promise.get_future();

    group.start();
    group.post([&promise]() {
        auto &loop = fiber::event::EventLoopGroup::current();
        auto *pool = fiber::async::CoroutineFramePool::current();
        bool ok = pool != nullptr && pool == &loop.frame_pool();
        promise.set_value(ok);
        loop.stop();
    });

    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "EventLoop thread did not process task in time";
        return;
    }

    EXPECT_TRUE(future.get());
    group.join();
}
