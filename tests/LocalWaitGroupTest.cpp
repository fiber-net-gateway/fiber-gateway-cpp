#include <gtest/gtest.h>

#include <chrono>
#include <future>

#include <fiber/async/LocalWaitGroup.h>
#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/event/EventLoopGroup.h>

namespace {

using DetachedTask = fiber::async::DetachedTask;
using LocalWaitGroup = fiber::async::LocalWaitGroup;
using namespace std::chrono_literals;

struct JoinObservation {
    bool resumed_after_last_done = false;
    std::size_t count_at_resume = 0;
};

DetachedTask join_empty_group(LocalWaitGroup *group, std::promise<bool> *promise) {
    co_await group->join();
    promise->set_value(group->empty());
}

DetachedTask done_later(LocalWaitGroup *group, std::chrono::milliseconds delay, bool *done_flag) {
    co_await fiber::async::sleep(delay);
    group->done();
    // Completion is posted, never inline: a joiner that resumes sees this set.
    *done_flag = true;
}

DetachedTask join_then_report(LocalWaitGroup *group, bool *done_flag, std::promise<JoinObservation> *promise) {
    JoinObservation observation{};
    co_await group->join();
    observation.resumed_after_last_done = *done_flag;
    observation.count_at_resume = group->count();
    promise->set_value(observation);
}

} // namespace

TEST(LocalWaitGroupTest, JoinOnEmptyGroupIsReady) {
    fiber::event::EventLoopGroup group(1);
    group.start();
    LocalWaitGroup wg;

    std::promise<bool> promise;
    auto future = promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return join_empty_group(&wg, &promise); });
    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(future.get());

    group.stop();
    group.join();
}

TEST(LocalWaitGroupTest, JoinResumesAfterCountReachesZero) {
    fiber::event::EventLoopGroup group(1);
    group.start();
    LocalWaitGroup wg;
    wg.add(2);

    bool done_flag = false;
    std::promise<JoinObservation> promise;
    auto future = promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return join_then_report(&wg, &done_flag, &promise); });
    fiber::async::spawn(group.at(0), [&]() { return done_later(&wg, 5ms, &done_flag); });
    fiber::async::spawn(group.at(0), [&]() { return done_later(&wg, 15ms, &done_flag); });

    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    auto observation = future.get();
    EXPECT_TRUE(observation.resumed_after_last_done);
    EXPECT_EQ(observation.count_at_resume, 0U);
    EXPECT_TRUE(wg.empty());
    EXPECT_FALSE(wg.has_waiters());

    group.stop();
    group.join();
}

TEST(LocalWaitGroupTest, MultipleJoinersAllResume) {
    fiber::event::EventLoopGroup group(1);
    group.start();
    LocalWaitGroup wg;
    wg.add();

    bool done_flag = false;
    std::promise<JoinObservation> first_promise;
    std::promise<JoinObservation> second_promise;
    auto first = first_promise.get_future();
    auto second = second_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return join_then_report(&wg, &done_flag, &first_promise); });
    fiber::async::spawn(group.at(0), [&]() { return join_then_report(&wg, &done_flag, &second_promise); });
    fiber::async::spawn(group.at(0), [&]() { return done_later(&wg, 5ms, &done_flag); });

    ASSERT_EQ(first.wait_for(2s), std::future_status::ready);
    ASSERT_EQ(second.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(first.get().count_at_resume, 0U);
    EXPECT_EQ(second.get().count_at_resume, 0U);
    EXPECT_FALSE(wg.has_waiters());

    group.stop();
    group.join();
}

TEST(LocalWaitGroupTest, ReusableAfterDraining) {
    fiber::event::EventLoopGroup group(1);
    group.start();
    LocalWaitGroup wg;

    for (int round = 0; round < 2; ++round) {
        wg.add();
        bool done_flag = false;
        std::promise<JoinObservation> promise;
        auto future = promise.get_future();
        fiber::async::spawn(group.at(0), [&]() { return join_then_report(&wg, &done_flag, &promise); });
        fiber::async::spawn(group.at(0), [&]() { return done_later(&wg, 5ms, &done_flag); });
        ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
        EXPECT_EQ(future.get().count_at_resume, 0U);
    }

    group.stop();
    group.join();
}
