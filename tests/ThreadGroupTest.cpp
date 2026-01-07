#include <gtest/gtest.h>

#include <vector>

#include "async/ThreadGroup.h"

TEST(ThreadGroupTest, CurrentMatchesThread) {
    constexpr std::size_t kThreads = 4;
    fiber::async::ThreadGroup group(kThreads);
    std::vector<fiber::async::ThreadGroup::Thread *> param_ptrs(kThreads, nullptr);
    std::vector<fiber::async::ThreadGroup::Thread *> current_ptrs(kThreads, nullptr);

    group.start([&](fiber::async::ThreadGroup::Thread &thread) {
        const auto index = thread.index();
        param_ptrs[index] = &thread;
        current_ptrs[index] = &fiber::async::ThreadGroup::Thread::current();
    });

    group.join();

    for (std::size_t i = 0; i < kThreads; ++i) {
        ASSERT_NE(param_ptrs[i], nullptr);
        ASSERT_EQ(param_ptrs[i], current_ptrs[i]);
    }

    for (std::size_t i = 0; i < kThreads; ++i) {
        for (std::size_t j = i + 1; j < kThreads; ++j) {
            EXPECT_NE(param_ptrs[i], param_ptrs[j]);
        }
    }
}

#if GTEST_HAS_DEATH_TEST
TEST(ThreadGroupTest, CurrentOutsideThreadAborts) {
    EXPECT_DEATH(fiber::async::ThreadGroup::Thread::current(), "FIBER_ASSERT failed");
}
#endif
