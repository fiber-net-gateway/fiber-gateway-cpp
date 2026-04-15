#include <gtest/gtest.h>

#include <chrono>
#include <future>

#include "async/Spawn.h"
#include "event/EventLoopGroup.h"

TEST(EventLoopTest, GroupIndexMatchesLoopOrder) {
    fiber::event::EventLoopGroup group(3);

    for (std::size_t i = 0; i < group.size(); ++i) {
        auto &loop = group.at(i);
        EXPECT_TRUE(loop.has_group_index());
        EXPECT_EQ(loop.group_index(), i);
    }
}

TEST(EventLoopTest, StandaloneLoopHasNoGroupIndex) {
    fiber::event::EventLoop loop;

    EXPECT_EQ(loop.group(), nullptr);
    EXPECT_FALSE(loop.has_group_index());
}
