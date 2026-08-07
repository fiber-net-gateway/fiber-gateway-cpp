#include <gtest/gtest.h>

#include <array>
#include <string_view>

#include <fiber/event/EventLoopGroup.h>
#include "fiber/prometheus/MetricsRegistry.h"

namespace {

using fiber::common::IoErr;
using fiber::prometheus::GaugeReduction;
using fiber::prometheus::HistogramUnit;
using fiber::prometheus::MetricsRegistry;

TEST(MetricsRegistryTest, ValidatesFamilyAndGeneratedSampleNames) {
    MetricsRegistry registry;

    EXPECT_EQ(registry.register_counter("requests", "invalid").error(), IoErr::Invalid);
    EXPECT_EQ(registry.register_gauge("9queue", "invalid", GaugeReduction::Sum).error(), IoErr::Invalid);

    auto histogram = registry.register_histogram("latency_seconds", "Latency", std::array<std::uint64_t, 2>{1, 2},
                                                 HistogramUnit::Microseconds);
    ASSERT_TRUE(histogram);
    EXPECT_EQ(registry.register_gauge("latency_seconds_count", "collision", GaugeReduction::Sum).error(),
              IoErr::Invalid);

    auto counter = registry.register_counter("requests_total", "Requests");
    ASSERT_TRUE(counter);
    EXPECT_EQ(registry.register_counter("requests_total", "duplicate").error(), IoErr::Invalid);
}

TEST(MetricsRegistryTest, ValidatesLabelsBucketsAndSeries) {
    MetricsRegistry registry;
    constexpr std::array<std::string_view, 2> labels{"method", "status"};
    auto counter = registry.register_counter("requests_total", "Requests", labels);
    ASSERT_TRUE(counter);

    constexpr std::array<std::string_view, 2> first_values{"GET", "2xx"};
    ASSERT_TRUE(registry.register_series(*counter, first_values));
    EXPECT_EQ(registry.register_series(*counter, first_values).error(), IoErr::Invalid);
    EXPECT_EQ(registry.register_series(*counter, std::array<std::string_view, 1>{"GET"}).error(), IoErr::Invalid);

    EXPECT_EQ(registry.register_gauge("bad_labels", "bad", GaugeReduction::Sum,
                                      std::array<std::string_view, 2>{"worker", "worker"})
                      .error(),
              IoErr::Invalid);
    EXPECT_EQ(registry.register_histogram("bad_hist", "bad", std::array<std::uint64_t, 3>{1, 1, 2}).error(),
              IoErr::Invalid);
    EXPECT_EQ(registry.register_histogram("reserved_hist", "bad", std::array<std::uint64_t, 2>{1, 2},
                                          HistogramUnit::Raw, std::array<std::string_view, 1>{"le"})
                      .error(),
              IoErr::Invalid);
    EXPECT_EQ(registry.register_histogram("duration_seconds", "bad", std::array<std::uint64_t, 2>{1, 2},
                                          HistogramUnit::Raw)
                      .error(),
              IoErr::Invalid);
    EXPECT_EQ(registry.register_histogram("duration_ticks", "bad", std::array<std::uint64_t, 2>{1, 2},
                                          HistogramUnit::Microseconds)
                      .error(),
              IoErr::Invalid);
}

TEST(MetricsRegistryTest, FreezeCreatesStableTypedShardHandles) {
    fiber::event::EventLoopGroup loops(2);
    MetricsRegistry registry;

    auto counter_family = registry.register_counter("requests_total", "Requests");
    auto gauge_family = registry.register_gauge("connections", "Connections", GaugeReduction::Sum);
    auto histogram_family = registry.register_histogram(
            "latency_seconds", "Latency", std::array<std::uint64_t, 3>{1, 5, 10}, HistogramUnit::Microseconds);
    ASSERT_TRUE(counter_family);
    ASSERT_TRUE(gauge_family);
    ASSERT_TRUE(histogram_family);

    auto counter_series = registry.register_series(*counter_family);
    auto gauge_series = registry.register_series(*gauge_family);
    auto histogram_series = registry.register_series(*histogram_family);
    auto shard0 = registry.add_shard(loops.at(0));
    auto shard1 = registry.add_shard(loops.at(1));
    ASSERT_TRUE(counter_series);
    ASSERT_TRUE(gauge_series);
    ASSERT_TRUE(histogram_series);
    ASSERT_TRUE(shard0);
    ASSERT_TRUE(shard1);
    ASSERT_TRUE(registry.freeze());

    auto *first = registry.shard(*shard0);
    auto *second = registry.shard(*shard1);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_NE(first, second);
    EXPECT_TRUE(first->counter(*counter_series));
    EXPECT_TRUE(first->gauge(*gauge_series));
    EXPECT_TRUE(first->histogram(*histogram_series));
    EXPECT_EQ(first->gauge(*counter_series).error(), IoErr::Invalid);
    EXPECT_TRUE(second->counter(*counter_series));

    EXPECT_TRUE(registry.frozen());
    EXPECT_EQ(registry.family_count(), 3);
    EXPECT_EQ(registry.shard_count(), 2);
    EXPECT_EQ(registry.freeze().error(), IoErr::Already);
    EXPECT_EQ(registry.register_series(*counter_family).error(), IoErr::Invalid);
    EXPECT_EQ(registry.add_shard(loops.at(0)).error(), IoErr::Invalid);
}

TEST(MetricsRegistryTest, RejectsIdsFromAnotherRegistryAndDuplicateOwnerLoop) {
    fiber::event::EventLoopGroup loops(1);
    MetricsRegistry first;
    MetricsRegistry second;

    auto family = first.register_counter("requests_total", "Requests");
    ASSERT_TRUE(family);
    EXPECT_EQ(second.register_series(*family).error(), IoErr::Invalid);

    ASSERT_TRUE(first.add_shard(loops.at(0)));
    EXPECT_EQ(first.add_shard(loops.at(0)).error(), IoErr::Invalid);
}

} // namespace
