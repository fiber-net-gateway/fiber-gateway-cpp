#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <common/Assert.h>

#include "discovery/LoadBalancer.h"

namespace {

using namespace std::chrono_literals;
using fiber::ai_server::DiscoveredInstance;
using fiber::ai_server::DiscoveredService;
using fiber::ai_server::InstanceReportOutcome;
using fiber::ai_server::LoadBalanceError;
using fiber::ai_server::LoadBalancer;
using fiber::ai_server::LoadBalancerUpdateResult;

DiscoveredInstance make_instance(std::string id, std::string_view address, std::uint16_t port, double weight) {
    fiber::net::IpAddress ip;
    FIBER_ASSERT(fiber::net::IpAddress::parse(address, ip));
    return DiscoveredInstance{
            .instance_id = std::move(id),
            .address = fiber::net::SocketAddress(ip, port),
            .connection_key = fiber::http::Http1ConnectionGroupKey::from_ip(
                    ip, port, fiber::http::Http1ConnectionGroupKey::Scheme::Http),
            .host_header = std::string(address) + ":" + std::to_string(port),
            .weight = weight,
            .cluster_name = "primary",
    };
}

DiscoveredService make_service(std::string checksum, std::vector<DiscoveredInstance> instances) {
    return DiscoveredService{
            .service_name = "backend",
            .group = "DEFAULT_GROUP",
            .checksum = std::move(checksum),
            .instances = std::move(instances),
    };
}

static_assert(std::is_move_constructible_v<LoadBalancer::Instance>);
static_assert(std::is_move_assignable_v<LoadBalancer::Instance>);
static_assert(!std::is_copy_constructible_v<LoadBalancer::Instance>);

TEST(LoadBalancerTest, SmoothWeightedRoundRobinUsesNormalizedNginxSequence) {
    LoadBalancer load_balancer;
    const auto now = LoadBalancer::TimePoint{};
    EXPECT_EQ(load_balancer.load_balance(now).error(), LoadBalanceError::Uninitialized);
    EXPECT_EQ(load_balancer.update_instances(make_service("v1", {make_instance("a", "10.0.0.1", 8080, 5.0),
                                                                 make_instance("b", "10.0.0.2", 8080, 1.0),
                                                                 make_instance("c", "10.0.0.3", 8080, 1.0)})),
              LoadBalancerUpdateResult::Applied);

    std::map<std::string, std::size_t> selected;
    for (std::size_t i = 0; i < 70; ++i) {
        auto instance = load_balancer.load_balance(now);
        ASSERT_TRUE(instance);
        ++selected[std::string(instance->instance_id())];
        load_balancer.report(std::move(*instance), InstanceReportOutcome::Success, now);
    }

    EXPECT_EQ(selected["a"], 50u);
    EXPECT_EQ(selected["b"], 10u);
    EXPECT_EQ(selected["c"], 10u);
    EXPECT_EQ(load_balancer.stats().success_reports, 70u);
}

TEST(LoadBalancerTest, WeightedRendezvousIsStableAndSupportsRequestExclusions) {
    LoadBalancer load_balancer;
    const auto now = LoadBalancer::TimePoint{};
    EXPECT_EQ(load_balancer.update_instances(make_service("v1", {make_instance("a", "10.0.0.1", 8080, 5.0),
                                                                 make_instance("b", "10.0.0.2", 8080, 1.0),
                                                                 make_instance("c", "10.0.0.3", 8080, 1.0)})),
              LoadBalancerUpdateResult::Applied);

    auto first = load_balancer.load_balance(42, {}, now);
    ASSERT_TRUE(first);
    const std::string first_name(first->instance_id());
    const std::uint64_t first_peer_id = first->peer_id();
    load_balancer.report(std::move(*first), InstanceReportOutcome::Neutral, now);

    for (std::size_t i = 0; i < 10; ++i) {
        auto repeated = load_balancer.load_balance(42, {}, now);
        ASSERT_TRUE(repeated);
        EXPECT_EQ(repeated->instance_id(), first_name);
        EXPECT_EQ(repeated->peer_id(), first_peer_id);
        load_balancer.report(std::move(*repeated), InstanceReportOutcome::Neutral, now);
    }

    const std::array<std::uint64_t, 1> excluded{first_peer_id};
    auto second = load_balancer.load_balance(42, excluded, now);
    ASSERT_TRUE(second);
    EXPECT_NE(second->instance_id(), first_name);
    load_balancer.report(std::move(*second), InstanceReportOutcome::Neutral, now);
}

TEST(LoadBalancerTest, WeightedRendezvousUsesNacosBaseWeightDistribution) {
    LoadBalancer load_balancer;
    const auto now = LoadBalancer::TimePoint{};
    EXPECT_EQ(load_balancer.update_instances(make_service("v1", {make_instance("a", "10.0.0.1", 8080, 5.0),
                                                                 make_instance("b", "10.0.0.2", 8080, 1.0),
                                                                 make_instance("c", "10.0.0.3", 8080, 1.0)})),
              LoadBalancerUpdateResult::Applied);

    constexpr std::size_t kSelections = 70'000;
    std::map<std::string, std::size_t> selected;
    for (std::uint64_t key = 0; key < kSelections; ++key) {
        auto instance = load_balancer.load_balance(key, {}, now);
        ASSERT_TRUE(instance);
        ++selected[std::string(instance->instance_id())];
        load_balancer.report(std::move(*instance), InstanceReportOutcome::Neutral, now);
    }

    EXPECT_NEAR(static_cast<double>(selected["a"]), 50'000.0, 1'000.0);
    EXPECT_NEAR(static_cast<double>(selected["b"]), 10'000.0, 600.0);
    EXPECT_NEAR(static_cast<double>(selected["c"]), 10'000.0, 600.0);
}

TEST(LoadBalancerTest, WeightedRendezvousMappingHasStableGoldenVectors) {
    LoadBalancer load_balancer;
    const auto now = LoadBalancer::TimePoint{};
    EXPECT_EQ(load_balancer.update_instances(make_service("v1", {make_instance("a", "10.0.0.1", 8080, 5.0),
                                                                 make_instance("b", "10.0.0.2", 8080, 3.0),
                                                                 make_instance("c", "10.0.0.3", 8080, 1.0)})),
              LoadBalancerUpdateResult::Applied);

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
        SCOPED_TRACE(::testing::Message() << "key=" << kKeys[i]);
        auto selected = load_balancer.load_balance(kKeys[i], {}, now);
        ASSERT_TRUE(selected);
        EXPECT_EQ(selected->instance_id(), kExpected[i]);
        load_balancer.report(std::move(*selected), InstanceReportOutcome::Neutral, now);
    }
}

TEST(LoadBalancerTest, WeightedRendezvousKeepsBaseWeightAffinityUntilCircuitOpens) {
    LoadBalancer load_balancer(LoadBalancer::Options{
            .max_fails = 2,
            .fail_timeout = 10s,
    });
    const auto now = LoadBalancer::TimePoint{};
    EXPECT_EQ(load_balancer.update_instances(make_service(
                      "v1", {make_instance("a", "10.0.0.1", 8080, 1.0), make_instance("b", "10.0.0.2", 8080, 1.0)})),
              LoadBalancerUpdateResult::Applied);

    auto first = load_balancer.load_balance(7, {}, now);
    ASSERT_TRUE(first);
    const std::string first_name(first->instance_id());
    load_balancer.report(std::move(*first), InstanceReportOutcome::Failure, now);

    auto before_open = load_balancer.load_balance(7, {}, now + 1ms);
    ASSERT_TRUE(before_open);
    EXPECT_EQ(before_open->instance_id(), first_name);
    load_balancer.report(std::move(*before_open), InstanceReportOutcome::Failure, now + 1ms);

    auto after_open = load_balancer.load_balance(7, {}, now + 2ms);
    ASSERT_TRUE(after_open);
    EXPECT_NE(after_open->instance_id(), first_name);
    load_balancer.report(std::move(*after_open), InstanceReportOutcome::Neutral, now + 2ms);
}

TEST(LoadBalancerTest, WeightedRendezvousOnlyMovesKeysToAnAddedPeer) {
    LoadBalancer load_balancer;
    const auto now = LoadBalancer::TimePoint{};
    EXPECT_EQ(load_balancer.update_instances(make_service(
                      "v1", {make_instance("a", "10.0.0.1", 8080, 1.0), make_instance("b", "10.0.0.2", 8080, 1.0)})),
              LoadBalancerUpdateResult::Applied);

    constexpr std::size_t kKeys = 1'000;
    std::array<std::string, kKeys> before;
    for (std::uint64_t key = 0; key < kKeys; ++key) {
        auto selected = load_balancer.load_balance(key, {}, now);
        ASSERT_TRUE(selected);
        before[key] = selected->instance_id();
        load_balancer.report(std::move(*selected), InstanceReportOutcome::Neutral, now);
    }

    EXPECT_EQ(load_balancer.update_instances(make_service("v2", {make_instance("a", "10.0.0.1", 8080, 1.0),
                                                                 make_instance("b", "10.0.0.2", 8080, 1.0),
                                                                 make_instance("c", "10.0.0.3", 8080, 1.0)})),
              LoadBalancerUpdateResult::Applied);
    std::size_t moved = 0;
    for (std::uint64_t key = 0; key < kKeys; ++key) {
        auto selected = load_balancer.load_balance(key, {}, now);
        ASSERT_TRUE(selected);
        if (selected->instance_id() != before[key]) {
            ++moved;
            EXPECT_EQ(selected->instance_id(), "c");
        }
        load_balancer.report(std::move(*selected), InstanceReportOutcome::Neutral, now);
    }
    EXPECT_GT(moved, 0u);
}

TEST(LoadBalancerTest, AtomicGenerationReplacementKeepsSelectedInstanceMemoryAlive) {
    LoadBalancer load_balancer;
    const auto now = LoadBalancer::TimePoint{};
    EXPECT_EQ(load_balancer.update_instances(make_service(
                      "v1", {make_instance("a", "10.0.0.1", 8080, 1.0), make_instance("b", "10.0.0.2", 8080, 1.0)})),
              LoadBalancerUpdateResult::Applied);
    auto old = load_balancer.load_balance(now);
    ASSERT_TRUE(old);
    EXPECT_EQ(old->generation(), 1u);

    EXPECT_EQ(load_balancer.update_instances(make_service("v2", {make_instance("c", "10.0.0.3", 9090, 1.0)})),
              LoadBalancerUpdateResult::Applied);
    EXPECT_EQ(load_balancer.generation(), 2u);
    EXPECT_EQ(old->address().ip().to_string(), "10.0.0.1");
    EXPECT_EQ(old->generation(), 1u);

    auto current = load_balancer.load_balance(now);
    ASSERT_TRUE(current);
    EXPECT_EQ(current->instance_id(), "c");
    EXPECT_EQ(current->generation(), 2u);
    load_balancer.report(std::move(*old), InstanceReportOutcome::Failure, now);
    load_balancer.report(std::move(*current), InstanceReportOutcome::Success, now);
}

TEST(LoadBalancerTest, PeerCircuitStateSurvivesInstanceWeightUpdates) {
    LoadBalancer load_balancer(LoadBalancer::Options{
            .max_fails = 1,
            .fail_timeout = 10s,
    });
    const auto now = LoadBalancer::TimePoint{};
    EXPECT_EQ(load_balancer.update_instances(make_service(
                      "v1", {make_instance("a", "10.0.0.1", 8080, 1.0), make_instance("b", "10.0.0.2", 8080, 1.0)})),
              LoadBalancerUpdateResult::Applied);
    auto failed = load_balancer.load_balance(now);
    ASSERT_TRUE(failed);
    ASSERT_EQ(failed->instance_id(), "a");
    load_balancer.report(std::move(*failed), InstanceReportOutcome::Failure, now);

    EXPECT_EQ(load_balancer.update_instances(make_service("v2", {make_instance("a-new-metadata", "10.0.0.1", 8080, 3.0),
                                                                 make_instance("b", "10.0.0.2", 8080, 1.0)})),
              LoadBalancerUpdateResult::Applied);
    auto selected = load_balancer.load_balance(now + 1ms);
    ASSERT_TRUE(selected);
    EXPECT_EQ(selected->instance_id(), "b");
    load_balancer.report(std::move(*selected), InstanceReportOutcome::Success, now + 1ms);
    EXPECT_EQ(load_balancer.stats().circuit_opens, 1u);
}

TEST(LoadBalancerTest, SinglePeerCircuitOpensAndRecoversAfterFailTimeout) {
    LoadBalancer load_balancer(LoadBalancer::Options{
            .max_fails = 2,
            .fail_timeout = 10s,
    });
    const auto now = LoadBalancer::TimePoint{};
    EXPECT_EQ(load_balancer.update_instances(make_service("v1", {make_instance("only", "10.0.0.1", 8080, 1.0)})),
              LoadBalancerUpdateResult::Applied);

    auto first = load_balancer.load_balance(now);
    ASSERT_TRUE(first);
    load_balancer.report(*first, false, now);
    auto second = load_balancer.load_balance(now + 1ms);
    ASSERT_TRUE(second);
    load_balancer.report(std::move(*second), InstanceReportOutcome::Failure, now + 1ms);

    auto open = load_balancer.load_balance(now + 2ms);
    ASSERT_FALSE(open);
    EXPECT_EQ(open.error(), LoadBalanceError::NoAvailableInstance);

    auto probe = load_balancer.load_balance(now + 11s);
    ASSERT_TRUE(probe);
    load_balancer.report(*probe, true, now + 11s);
    auto recovered = load_balancer.load_balance(now + 11s);
    ASSERT_TRUE(recovered);
    load_balancer.report(std::move(*recovered), InstanceReportOutcome::Success, now + 11s);
}

TEST(LoadBalancerTest, UnreportedInstanceIsReleasedAsNeutral) {
    LoadBalancer load_balancer;
    EXPECT_EQ(load_balancer.update_instances(make_service("v1", {make_instance("only", "10.0.0.1", 8080, 1.0)})),
              LoadBalancerUpdateResult::Applied);
    {
        auto selected = load_balancer.load_balance(LoadBalancer::TimePoint{});
        ASSERT_TRUE(selected);
    }
    EXPECT_EQ(load_balancer.stats().neutral_reports, 1u);
}

TEST(LoadBalancerTest, OneSharedBalancerSerializesWorkersAcrossAtomicUpdates) {
    LoadBalancer load_balancer;
    EXPECT_EQ(load_balancer.update_instances(make_service(
                      "v1", {make_instance("a", "10.0.0.1", 8080, 2.0), make_instance("b", "10.0.0.2", 8080, 1.0)})),
              LoadBalancerUpdateResult::Applied);

    constexpr std::size_t kWorkers = 4;
    constexpr std::size_t kSelectionsPerWorker = 10'000;
    std::barrier start(static_cast<std::ptrdiff_t>(kWorkers + 1));
    std::atomic<bool> invalid{false};
    std::array<std::thread, kWorkers> workers;
    for (std::thread &worker: workers) {
        worker = std::thread([&] {
            start.arrive_and_wait();
            for (std::size_t i = 0; i < kSelectionsPerWorker; ++i) {
                auto selected = load_balancer.load_balance(LoadBalancer::TimePoint{} + 1ms);
                if (!selected || (selected->instance_id() != "a" && selected->instance_id() != "b" &&
                                  selected->instance_id() != "c")) {
                    invalid.store(true, std::memory_order_relaxed);
                    continue;
                }
                load_balancer.report(std::move(*selected), InstanceReportOutcome::Neutral,
                                     LoadBalancer::TimePoint{} + 1ms);
            }
        });
    }

    start.arrive_and_wait();
    for (std::size_t i = 0; i < 200; ++i) {
        const bool odd = (i & 1U) != 0;
        EXPECT_EQ(
                load_balancer.update_instances(make_service(
                        odd ? "odd" : "even", {make_instance(odd ? "c" : "a", odd ? "10.0.0.3" : "10.0.0.1", 8080, 1.0),
                                               make_instance("b", "10.0.0.2", 8080, 1.0)})),
                LoadBalancerUpdateResult::Applied);
    }
    for (std::thread &worker: workers) {
        worker.join();
    }

    EXPECT_FALSE(invalid.load(std::memory_order_relaxed));
    EXPECT_EQ(load_balancer.stats().selections, kWorkers * kSelectionsPerWorker);
}

} // namespace
