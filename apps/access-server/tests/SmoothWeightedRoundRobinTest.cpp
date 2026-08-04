#include <gtest/gtest.h>

#include <chrono>
#include <map>
#include <string>
#include <utility>

#include "runtime/SmoothWeightedRoundRobin.h"

namespace {

using namespace std::chrono_literals;
using TestSwrr = fiber::access_server::SmoothWeightedRoundRobin<std::string>;
using fiber::access_server::SwrrSelectError;

TestSwrr::WeightedInstance instance(std::uint64_t selection_token, std::string value, double weight) {
    return TestSwrr::WeightedInstance{
            .selection_token = selection_token,
            .instance = std::move(value),
            .weight = weight,
    };
}

TEST(SmoothWeightedRoundRobinTest, UsesNormalizedNginxSequence) {
    TestSwrr state;
    EXPECT_TRUE(state.update(
            {instance(1, "10.0.0.1:8080", 5.0), instance(2, "10.0.0.2:8080", 1.0), instance(3, "10.0.0.3:8080", 1.0)}));

    std::map<std::string, std::size_t> selected;
    for (std::size_t i = 0; i < 70; ++i) {
        auto result = state.select({}, TestSwrr::TimePoint{});
        ASSERT_TRUE(result);
        ++selected[result->instance()];
        result->report(true, TestSwrr::TimePoint{});
    }
    EXPECT_EQ(selected["10.0.0.1:8080"], 50U);
    EXPECT_EQ(selected["10.0.0.2:8080"], 10U);
    EXPECT_EQ(selected["10.0.0.3:8080"], 10U);
}

TEST(SmoothWeightedRoundRobinTest, SupportsRequestLevelExclusion) {
    TestSwrr state;
    EXPECT_TRUE(state.update({instance(101, "10.0.0.1:8080", 10.0), instance(102, "10.0.0.2:8080", 1.0)}));

    auto first = state.select({}, TestSwrr::TimePoint{});
    ASSERT_TRUE(first);
    EXPECT_EQ(first->selection_token(), 101U);
    const std::uint64_t excluded = first->selection_token();
    first->report(false, TestSwrr::TimePoint{});

    auto retry = state.select(std::span(&excluded, 1), TestSwrr::TimePoint{});
    ASSERT_TRUE(retry);
    EXPECT_EQ(retry->selection_token(), 102U);
}

TEST(SmoothWeightedRoundRobinTest, CircuitStateSurvivesUpdatesAndRecovers) {
    TestSwrr state(TestSwrr::Options{
            .max_fails = 1,
            .fail_timeout = 10s,
    });
    EXPECT_TRUE(state.update({instance(1, "10.0.0.1:8080", 1.0), instance(2, "10.0.0.2:8080", 1.0)}));
    auto failed = state.select({}, TestSwrr::TimePoint{});
    ASSERT_TRUE(failed);
    EXPECT_EQ(failed->selection_token(), 1U);
    failed->report(false, TestSwrr::TimePoint{});

    EXPECT_TRUE(state.update({instance(1, "10.0.0.1:8080", 3.0), instance(2, "10.0.0.2:8080", 1.0)}));
    auto selected = state.select({}, TestSwrr::TimePoint{} + 1ms);
    ASSERT_TRUE(selected);
    EXPECT_EQ(selected->selection_token(), 2U);

    auto recovered = state.select({}, TestSwrr::TimePoint{} + 11s);
    ASSERT_TRUE(recovered);
}

TEST(SmoothWeightedRoundRobinTest, SelectionPinsOldGenerationAndInstance) {
    TestSwrr state;
    EXPECT_TRUE(state.update({instance(1, "orders.internal:8080", 1.0)}));
    auto old = state.select({}, TestSwrr::TimePoint{});
    ASSERT_TRUE(old);

    EXPECT_TRUE(state.update({instance(2, "10.0.0.2:9090", 1.0)}));
    EXPECT_EQ(old->instance(), "orders.internal:8080");
    EXPECT_EQ(old->generation(), 1U);

    auto current = state.select({}, TestSwrr::TimePoint{});
    ASSERT_TRUE(current);
    EXPECT_EQ(current->instance(), "10.0.0.2:9090");
    EXPECT_EQ(current->generation(), 2U);
}

TEST(SmoothWeightedRoundRobinTest, EmptyInstanceSetFailsClosed) {
    TestSwrr state;
    EXPECT_TRUE(state.update({}));
    auto selected = state.select({}, TestSwrr::TimePoint{});
    ASSERT_FALSE(selected);
    EXPECT_EQ(selected.error(), SwrrSelectError::NoAvailableInstance);
}

} // namespace
