#include <gtest/gtest.h>

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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

TEST(SmoothWeightedRoundRobinTest, LateReportUpdatesRetainedInstanceAfterMultipleUpdates) {
    TestSwrr state(TestSwrr::Options{
            .max_fails = 1,
            .fail_timeout = 10s,
    });
    EXPECT_TRUE(state.update({instance(2, "10.0.0.2:8080", 1.0), instance(3, "10.0.0.3:8080", 1.0)}));
    auto old = state.select({}, TestSwrr::TimePoint{});
    ASSERT_TRUE(old);
    ASSERT_EQ(old->selection_token(), 2U);

    EXPECT_TRUE(state.update(
            {instance(1, "10.0.0.1:8080", 1.0), instance(2, "10.0.0.2:8080", 1.0), instance(3, "10.0.0.3:8080", 1.0)}));
    EXPECT_TRUE(state.update(
            {instance(1, "10.0.0.1:8080", 1.0), instance(2, "10.0.0.2:8080", 1.0), instance(4, "10.0.0.4:8080", 1.0)}));
    old->report(false, TestSwrr::TimePoint{});

    auto current = state.select({}, TestSwrr::TimePoint{} + 1ms);
    ASSERT_TRUE(current);
    EXPECT_NE(current->selection_token(), 2U);
}

TEST(SmoothWeightedRoundRobinTest, LateReportDoesNotAffectRecreatedInstance) {
    TestSwrr state(TestSwrr::Options{
            .max_fails = 1,
            .fail_timeout = 10s,
    });
    EXPECT_TRUE(state.update({instance(7, "10.0.0.1:8080", 1.0)}));
    auto removed = state.select({}, TestSwrr::TimePoint{});
    ASSERT_TRUE(removed);

    EXPECT_TRUE(state.update({instance(8, "10.0.0.1:8080", 1.0)}));
    removed->report(false, TestSwrr::TimePoint{});

    auto recreated = state.select({}, TestSwrr::TimePoint{} + 1ms);
    ASSERT_TRUE(recreated);
    EXPECT_EQ(recreated->selection_token(), 8U);
}

TEST(SmoothWeightedRoundRobinTest, SelectionKeepsInstanceAliveAfterBalancerDestruction) {
    TestSwrr::Selection selected;
    {
        auto state = std::make_unique<TestSwrr>();
        EXPECT_TRUE(state->update({instance(1, "orders.internal:8080", 1.0)}));
        auto result = state->select({}, TestSwrr::TimePoint{});
        ASSERT_TRUE(result);
        selected = std::move(*result);
    }

    ASSERT_TRUE(selected.valid());
    EXPECT_EQ(selected.instance(), "orders.internal:8080");
    selected.report(false, TestSwrr::TimePoint{});
    EXPECT_FALSE(selected.pending());
    EXPECT_TRUE(selected.valid());
}

TEST(SmoothWeightedRoundRobinTest, IdenticalUpdateDoesNotAdvanceGeneration) {
    TestSwrr state;
    EXPECT_TRUE(state.update({instance(1, "10.0.0.1:8080", 1.0)}));
    EXPECT_FALSE(state.update({instance(1, "10.0.0.1:8080", 1.0)}));
    EXPECT_EQ(state.generation(), 1U);
}

TEST(SmoothWeightedRoundRobinTest, TokenMappingResolvesCollisionsAcrossGeneration) {
    TestSwrr state(TestSwrr::Options{
            .max_fails = 1,
            .fail_timeout = 10s,
    });
    std::vector<TestSwrr::WeightedInstance> configured;
    for (std::uint64_t token = 1; token <= 32; ++token) {
        configured.push_back(instance(token, "instance-" + std::to_string(token), 1.0));
    }
    EXPECT_TRUE(state.update(std::move(configured)));

    std::vector<TestSwrr::Selection> selected;
    selected.reserve(32);
    std::vector<std::uint64_t> excluded;
    excluded.reserve(31);
    for (std::uint64_t selected_token = 1; selected_token <= 32; ++selected_token) {
        excluded.clear();
        for (std::uint64_t token = 1; token <= 32; ++token) {
            if (token != selected_token) {
                excluded.push_back(token);
            }
        }
        auto result = state.select(excluded, TestSwrr::TimePoint{});
        ASSERT_TRUE(result);
        ASSERT_EQ(result->selection_token(), selected_token);
        selected.push_back(std::move(*result));
    }

    configured.clear();
    for (std::uint64_t token = 1; token <= 33; ++token) {
        configured.push_back(instance(token, "instance-" + std::to_string(token), 1.0));
    }
    EXPECT_TRUE(state.update(std::move(configured)));
    for (TestSwrr::Selection &selection: selected) {
        selection.report(false, TestSwrr::TimePoint{});
    }

    const std::uint64_t new_token = 33;
    auto unavailable = state.select(std::span(&new_token, 1), TestSwrr::TimePoint{} + 1ms);
    EXPECT_FALSE(unavailable);
    EXPECT_EQ(unavailable.error(), SwrrSelectError::NoAvailableInstance);
}

TEST(SmoothWeightedRoundRobinTest, EmptyInstanceSetFailsClosed) {
    TestSwrr state;
    EXPECT_TRUE(state.update({}));
    auto selected = state.select({}, TestSwrr::TimePoint{});
    ASSERT_FALSE(selected);
    EXPECT_EQ(selected.error(), SwrrSelectError::NoConfiguredInstance);
}

} // namespace
