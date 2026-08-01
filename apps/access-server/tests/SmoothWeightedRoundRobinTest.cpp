#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <map>
#include <string>
#include <utility>

#include "../../../tests/NacosSnapshotTestBuilder.h"
#include "runtime/SmoothWeightedRoundRobin.h"

namespace {

using namespace std::chrono_literals;
using fiber::access_server::ServiceSelectError;
using fiber::access_server::SmoothWeightedRoundRobin;

fiber::nacos::Instance instance(std::string id, std::string host, std::uint16_t port, double weight,
                                std::string cluster = "default") {
    return fiber::nacos::Instance{
            .instance_id = std::move(id),
            .ip = std::move(host),
            .port = port,
            .weight = weight,
            .cluster_name = std::move(cluster),
    };
}

std::shared_ptr<const fiber::nacos::ServiceInfo> service(std::string checksum,
                                                         std::vector<fiber::nacos::Instance> instances) {
    fiber::tests::ServiceInfoTestData data;
    data.name = "backend";
    data.group_name = "DEFAULT_GROUP";
    data.checksum = std::move(checksum);
    data.hosts = std::move(instances);
    return fiber::tests::make_service_info(std::move(data));
}

TEST(SmoothWeightedRoundRobinTest, UsesNormalizedNginxSequence) {
    SmoothWeightedRoundRobin state;
    const auto snapshot = service("v1", {instance("a", "10.0.0.1", 8080, 5.0), instance("b", "10.0.0.2", 8080, 1.0),
                                         instance("c", "10.0.0.3", 8080, 1.0)});
    EXPECT_TRUE(state.update(*snapshot));

    std::map<std::string, std::size_t> selected;
    for (std::size_t i = 0; i < 70; ++i) {
        auto result = state.select("default", "", {}, SmoothWeightedRoundRobin::TimePoint{});
        ASSERT_TRUE(result);
        ++selected[std::string(result->instance_id())];
        result->report(true, SmoothWeightedRoundRobin::TimePoint{});
    }
    EXPECT_EQ(selected["a"], 50U);
    EXPECT_EQ(selected["b"], 10U);
    EXPECT_EQ(selected["c"], 10U);
}

TEST(SmoothWeightedRoundRobinTest, PrefersZoneAndSupportsExclusion) {
    SmoothWeightedRoundRobin state;
    const auto snapshot = service("v1", {instance("local", "10.0.0.1", 8080, 1.0, "sh-default"),
                                         instance("remote", "10.0.0.2", 8080, 10.0, "bj-default"),
                                         instance("gray", "10.0.0.3", 8080, 1.0, "sh-gray")});
    EXPECT_TRUE(state.update(*snapshot));

    auto local = state.select("default", "sh", {}, SmoothWeightedRoundRobin::TimePoint{});
    ASSERT_TRUE(local);
    EXPECT_EQ(local->instance_id(), "local");
    const std::uint64_t excluded = local->peer_id();
    local->report(true, SmoothWeightedRoundRobin::TimePoint{});

    auto remote = state.select("default", "sh", std::span(&excluded, 1), SmoothWeightedRoundRobin::TimePoint{});
    ASSERT_TRUE(remote);
    EXPECT_EQ(remote->instance_id(), "remote");

    auto gray = state.select("gray", "sh", {}, SmoothWeightedRoundRobin::TimePoint{});
    ASSERT_TRUE(gray);
    EXPECT_EQ(gray->instance_id(), "gray");
}

TEST(SmoothWeightedRoundRobinTest, CircuitStateSurvivesUpdatesAndRecovers) {
    SmoothWeightedRoundRobin state(SmoothWeightedRoundRobin::Options{
            .max_fails = 1,
            .fail_timeout = 10s,
    });
    auto initial = service("v1", {instance("a", "10.0.0.1", 8080, 1.0), instance("b", "10.0.0.2", 8080, 1.0)});
    EXPECT_TRUE(state.update(*initial));
    auto failed = state.select("default", "", {}, SmoothWeightedRoundRobin::TimePoint{});
    ASSERT_TRUE(failed);
    EXPECT_EQ(failed->instance_id(), "a");
    failed->report(false, SmoothWeightedRoundRobin::TimePoint{});

    auto changed = service("v2", {instance("a-new", "10.0.0.1", 8080, 3.0), instance("b", "10.0.0.2", 8080, 1.0)});
    EXPECT_TRUE(state.update(*changed));
    auto selected = state.select("default", "", {}, SmoothWeightedRoundRobin::TimePoint{} + 1ms);
    ASSERT_TRUE(selected);
    EXPECT_EQ(selected->instance_id(), "b");

    auto recovered = state.select("default", "", {}, SmoothWeightedRoundRobin::TimePoint{} + 11s);
    ASSERT_TRUE(recovered);
}

TEST(SmoothWeightedRoundRobinTest, SelectionPinsOldGenerationAndHostname) {
    SmoothWeightedRoundRobin state;
    auto initial = service("v1", {instance("old", "orders.internal", 8080, 1.0)});
    EXPECT_TRUE(state.update(*initial));
    auto old = state.select("default", "", {}, SmoothWeightedRoundRobin::TimePoint{});
    ASSERT_TRUE(old);

    auto changed = service("v2", {instance("new", "10.0.0.2", 9090, 1.0)});
    EXPECT_TRUE(state.update(*changed));
    EXPECT_EQ(old->host(), "orders.internal");
    EXPECT_EQ(old->authority(), "orders.internal:8080");
    EXPECT_FALSE(old->ip_address());
    EXPECT_EQ(old->generation(), 1U);

    auto current = state.select("default", "", {}, SmoothWeightedRoundRobin::TimePoint{});
    ASSERT_TRUE(current);
    EXPECT_EQ(current->instance_id(), "new");
    EXPECT_EQ(current->generation(), 2U);
}

TEST(SmoothWeightedRoundRobinTest, EmptySnapshotFailsClosed) {
    SmoothWeightedRoundRobin state;
    const auto empty = service("v1", {});
    EXPECT_TRUE(state.update(*empty));
    auto selected = state.select("default", "", {}, SmoothWeightedRoundRobin::TimePoint{});
    ASSERT_FALSE(selected);
    EXPECT_EQ(selected.error(), ServiceSelectError::NoAvailableInstance);
}

} // namespace
