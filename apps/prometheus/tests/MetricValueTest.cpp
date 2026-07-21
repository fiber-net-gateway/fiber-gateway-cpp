#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "fiber/prometheus/Counter.h"
#include "fiber/prometheus/Gauge.h"
#include "fiber/prometheus/Histogram.h"

namespace {

TEST(MetricValueTest, CounterUsesPlainIntegerUpdates) {
    std::uint64_t value = 0;
    fiber::prometheus::CounterRef counter(value);

    counter.inc();
    counter.add(9);

    EXPECT_TRUE(counter.valid());
    EXPECT_EQ(value, 10);
}

TEST(MetricValueTest, GaugeSupportsSetAndSignedUpdates) {
    std::int64_t value = 0;
    fiber::prometheus::GaugeRef gauge(value);

    gauge.set(7);
    gauge.inc();
    gauge.add(-3);
    gauge.dec();

    EXPECT_TRUE(gauge.valid());
    EXPECT_EQ(value, 4);
}

TEST(MetricValueTest, HistogramUpdatesOneIntervalBucket) {
    constexpr std::array<std::uint64_t, 3> bounds{10, 20, 50};
    std::array<std::uint64_t, 3> intervals{};
    std::uint64_t count = 0;
    std::uint64_t sum = 0;
    fiber::prometheus::HistogramRef histogram(bounds, intervals, count, sum);

    histogram.observe(1);
    histogram.observe(10);
    histogram.observe(11);
    histogram.observe(50);
    histogram.observe(51);

    EXPECT_TRUE(histogram.valid());
    EXPECT_EQ(histogram.bucket_count(), bounds.size());
    EXPECT_EQ(intervals, (std::array<std::uint64_t, 3>{2, 1, 1}));
    EXPECT_EQ(count, 5);
    EXPECT_EQ(sum, 123);
}

TEST(MetricValueTest, DefaultHandlesAreInvalid) {
    EXPECT_FALSE(fiber::prometheus::CounterRef{}.valid());
    EXPECT_FALSE(fiber::prometheus::GaugeRef{}.valid());
    EXPECT_FALSE(fiber::prometheus::HistogramRef{}.valid());
}

} // namespace
