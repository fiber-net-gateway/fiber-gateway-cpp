#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <map>
#include <string>
#include <string_view>
#include <utility>

#include "../../../tests/NacosSnapshotTestBuilder.h"
#include "discovery/WeightedRendezvous.h"

namespace {

using namespace std::chrono_literals;
using fiber::ai_server::InstanceReportOutcome;
using fiber::ai_server::ServiceSelectError;
using fiber::ai_server::WeightedRendezvous;

fiber::nacos::Instance instance(std::string id, std::string host, std::uint16_t port, double weight) {
    return fiber::nacos::Instance{
            .instance_id = std::move(id),
            .ip = std::move(host),
            .port = port,
            .weight = weight,
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

TEST(WeightedRendezvousTest, IsStableAndSupportsExclusion) {
    WeightedRendezvous state;
    const auto snapshot = service("v1", {instance("a", "10.0.0.1", 8080, 5.0), instance("b", "10.0.0.2", 8080, 1.0),
                                         instance("c", "10.0.0.3", 8080, 1.0)});
    EXPECT_TRUE(state.update(*snapshot));

    auto first = state.select(42, {}, WeightedRendezvous::TimePoint{});
    ASSERT_TRUE(first);
    const std::string selected(first->instance_id());
    const std::uint64_t excluded = first->peer_id();
    first->report(InstanceReportOutcome::Neutral, WeightedRendezvous::TimePoint{});

    auto repeated = state.select(42, {}, WeightedRendezvous::TimePoint{});
    ASSERT_TRUE(repeated);
    EXPECT_EQ(repeated->instance_id(), selected);
    repeated->report(InstanceReportOutcome::Neutral, WeightedRendezvous::TimePoint{});

    auto alternative = state.select(42, std::span(&excluded, 1), WeightedRendezvous::TimePoint{});
    ASSERT_TRUE(alternative);
    EXPECT_NE(alternative->instance_id(), selected);
}

TEST(WeightedRendezvousTest, UsesNacosWeights) {
    WeightedRendezvous state;
    const auto snapshot = service("v1", {instance("a", "10.0.0.1", 8080, 5.0), instance("b", "10.0.0.2", 8080, 1.0),
                                         instance("c", "10.0.0.3", 8080, 1.0)});
    EXPECT_TRUE(state.update(*snapshot));

    constexpr std::size_t kSelections = 70'000;
    std::map<std::string, std::size_t> selected;
    for (std::uint64_t key = 0; key < kSelections; ++key) {
        auto result = state.select(key, {}, WeightedRendezvous::TimePoint{});
        ASSERT_TRUE(result);
        ++selected[std::string(result->instance_id())];
        result->report(InstanceReportOutcome::Neutral, WeightedRendezvous::TimePoint{});
    }
    EXPECT_NEAR(static_cast<double>(selected["a"]), 50'000.0, 1'000.0);
    EXPECT_NEAR(static_cast<double>(selected["b"]), 10'000.0, 600.0);
    EXPECT_NEAR(static_cast<double>(selected["c"]), 10'000.0, 600.0);
}

TEST(WeightedRendezvousTest, KeepsStableGoldenMapping) {
    WeightedRendezvous state;
    const auto snapshot = service("v1", {instance("a", "10.0.0.1", 8080, 5.0), instance("b", "10.0.0.2", 8080, 3.0),
                                         instance("c", "10.0.0.3", 8080, 1.0)});
    EXPECT_TRUE(state.update(*snapshot));
    constexpr std::array<std::uint64_t, 16> kKeys{
            0,
            1,
            2,
            3,
            7,
            42,
            99,
            1024,
            0x0123456789abcdefULL,
            0x1111111111111111ULL,
            0x5555555555555555ULL,
            0x8000000000000000ULL,
            0xaaaaaaaaaaaaaaaaULL,
            0xfedcba9876543210ULL,
            0xfffffffffffffffeULL,
            0xffffffffffffffffULL,
    };
    constexpr std::array<std::string_view, kKeys.size()> kExpected{
            "a", "b", "c", "a", "a", "a", "a", "a", "b", "a", "b", "a", "a", "a", "b", "b",
    };
    for (std::size_t i = 0; i < kKeys.size(); ++i) {
        auto selected = state.select(kKeys[i], {}, WeightedRendezvous::TimePoint{});
        ASSERT_TRUE(selected);
        EXPECT_EQ(selected->instance_id(), kExpected[i]);
    }
}

TEST(WeightedRendezvousTest, OnlyMovesKeysToAnAddedPeer) {
    WeightedRendezvous state;
    auto initial = service("v1", {instance("a", "10.0.0.1", 8080, 1.0), instance("b", "10.0.0.2", 8080, 1.0)});
    EXPECT_TRUE(state.update(*initial));
    constexpr std::size_t kKeys = 1'000;
    std::array<std::string, kKeys> before;
    for (std::uint64_t key = 0; key < kKeys; ++key) {
        auto selected = state.select(key, {}, WeightedRendezvous::TimePoint{});
        ASSERT_TRUE(selected);
        before[key] = selected->instance_id();
    }

    auto changed = service("v2", {instance("a", "10.0.0.1", 8080, 1.0), instance("b", "10.0.0.2", 8080, 1.0),
                                  instance("c", "10.0.0.3", 8080, 1.0)});
    EXPECT_TRUE(state.update(*changed));
    for (std::uint64_t key = 0; key < kKeys; ++key) {
        auto selected = state.select(key, {}, WeightedRendezvous::TimePoint{});
        ASSERT_TRUE(selected);
        if (selected->instance_id() != before[key]) {
            EXPECT_EQ(selected->instance_id(), "c");
        }
    }
}

TEST(WeightedRendezvousTest, CircuitOpensAndGenerationIsPinned) {
    WeightedRendezvous state(WeightedRendezvous::Options{
            .max_fails = 1,
            .fail_timeout = 10s,
    });
    auto initial = service("v1", {instance("a", "10.0.0.1", 8080, 1.0), instance("b", "10.0.0.2", 8080, 1.0)});
    EXPECT_TRUE(state.update(*initial));
    auto failed = state.select(7, {}, WeightedRendezvous::TimePoint{});
    ASSERT_TRUE(failed);
    const std::string failed_id(failed->instance_id());
    failed->report(InstanceReportOutcome::Failure, WeightedRendezvous::TimePoint{});

    auto fallback = state.select(7, {}, WeightedRendezvous::TimePoint{} + 1ms);
    ASSERT_TRUE(fallback);
    EXPECT_NE(fallback->instance_id(), failed_id);

    auto old = state.select(42, {}, WeightedRendezvous::TimePoint{} + 1ms);
    ASSERT_TRUE(old);
    auto changed = service("v2", {instance("new", "10.0.0.9", 9090, 1.0)});
    EXPECT_TRUE(state.update(*changed));
    EXPECT_EQ(old->generation(), 1U);
    EXPECT_NE(old->instance_id(), "new");
}

TEST(WeightedRendezvousTest, RejectsHostnameAndEmptySnapshots) {
    WeightedRendezvous state;
    auto snapshot = service("v1", {instance("hostname", "provider.internal", 8080, 1.0)});
    EXPECT_TRUE(state.update(*snapshot));
    EXPECT_EQ(state.configured_instance_count(), 0U);
    auto selected = state.select(1, {}, WeightedRendezvous::TimePoint{});
    ASSERT_FALSE(selected);
    EXPECT_EQ(selected.error(), ServiceSelectError::NoAvailableInstance);
}

} // namespace
