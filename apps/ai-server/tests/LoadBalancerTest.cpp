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
