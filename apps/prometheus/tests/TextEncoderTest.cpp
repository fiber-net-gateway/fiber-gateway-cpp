#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstring>
#include <string>
#include <sys/uio.h>

#include "PrometheusInternal.h"
#include "TextEncoder.h"

namespace {

using fiber::common::IoErr;
using fiber::prometheus::CollectOptions;
using fiber::prometheus::GaugeReduction;
using fiber::prometheus::HistogramUnit;
using fiber::prometheus::MetricType;
using fiber::prometheus::detail::FamilySchema;
using fiber::prometheus::detail::RegistryData;
using fiber::prometheus::detail::SeriesSchema;

std::string chain_string(const fiber::mem::IoBufChain &chain) {
    std::array<iovec, 64> iov{};
    const int count = chain.fill_write_iov(iov.data(), static_cast<int>(iov.size()));
    std::string result;
    for (int index = 0; index < count; ++index) {
        result.append(static_cast<const char *>(iov[index].iov_base), iov[index].iov_len);
    }
    return result;
}

FamilySchema family(MetricType type, std::string name, std::string help, std::size_t offset) {
    FamilySchema result;
    result.type = type;
    result.name = std::move(name);
    result.help = std::move(help);
    result.series.push_back(SeriesSchema{.word_offset = offset});
    return result;
}

TEST(TextEncoderTest, EncodesMetadataEscapesLabelsAndGaugeReductionsInOrder) {
    RegistryData data({});

    auto counter = family(MetricType::Counter, "requests_total", "Requests\\\nall", 0);
    counter.label_names = {"method"};
    counter.series[0].label_values = {"G\"ET\n\\"};

    auto minimum = family(MetricType::Gauge, "queue_min", "Minimum queue", 1);
    minimum.reduction = GaugeReduction::Min;
    auto maximum = family(MetricType::Gauge, "queue_max", "Maximum queue", 2);
    maximum.reduction = GaugeReduction::Max;
    auto sum = family(MetricType::Gauge, "queue_sum", "Queue sum", 3);
    sum.reduction = GaugeReduction::Sum;

    data.families = {counter, minimum, maximum, sum};
    data.word_count = 4;
    data.snapshots = {{4, std::bit_cast<std::uint64_t>(std::int64_t{-2}),
                       std::bit_cast<std::uint64_t>(std::int64_t{-2}), std::bit_cast<std::uint64_t>(std::int64_t{-2})},
                      {6, std::bit_cast<std::uint64_t>(std::int64_t{5}), std::bit_cast<std::uint64_t>(std::int64_t{5}),
                       std::bit_cast<std::uint64_t>(std::int64_t{5})}};

    fiber::mem::IoBuf out = fiber::mem::IoBuf::allocate(4096);
    auto encoded = fiber::prometheus::detail::encode_text_into(data, out, {});
    ASSERT_TRUE(encoded);
    std::string_view text(reinterpret_cast<const char *>(out.readable_data()), out.readable());
    EXPECT_EQ(text, "# HELP requests_total Requests\\\\\\nall\n"
                    "# TYPE requests_total counter\n"
                    "requests_total{method=\"G\\\"ET\\n\\\\\"} 10\n"
                    "# HELP queue_min Minimum queue\n"
                    "# TYPE queue_min gauge\n"
                    "queue_min -2\n"
                    "# HELP queue_max Maximum queue\n"
                    "# TYPE queue_max gauge\n"
                    "queue_max 5\n"
                    "# HELP queue_sum Queue sum\n"
                    "# TYPE queue_sum gauge\n"
                    "queue_sum 3\n");
}

TEST(TextEncoderTest, EncodesCumulativeHistogramAndExactDurationSeconds) {
    RegistryData data({});
    auto histogram = family(MetricType::Histogram, "request_duration_seconds", "Request duration.", 0);
    histogram.histogram_unit = HistogramUnit::Microseconds;
    histogram.upper_bounds = {1, 5};
    histogram.label_names = {"method"};
    histogram.series[0].label_values = {"GET"};

    data.families = {histogram};
    data.word_count = 4;
    data.snapshots = {{2, 1, 4, 12}, {1, 2, 5, 20}};

    fiber::mem::IoBufNodePool pool;
    auto encoded = fiber::prometheus::detail::encode_text_chain(data, pool, CollectOptions{.chunk_size = 7});
    ASSERT_TRUE(encoded);
    EXPECT_FALSE(encoded->complete());
    EXPECT_GT(encoded->size(), 1);
    EXPECT_EQ(chain_string(*encoded), "# HELP request_duration_seconds Request duration.\n"
                                      "# TYPE request_duration_seconds histogram\n"
                                      "request_duration_seconds_bucket{method=\"GET\",le=\"0.000001\"} 3\n"
                                      "request_duration_seconds_bucket{method=\"GET\",le=\"0.000005\"} 6\n"
                                      "request_duration_seconds_bucket{method=\"GET\",le=\"+Inf\"} 9\n"
                                      "request_duration_seconds_sum{method=\"GET\"} 0.000032\n"
                                      "request_duration_seconds_count{method=\"GET\"} 9\n");
}

TEST(TextEncoderTest, FixedBufferFailureDoesNotCommitPartialOutput) {
    RegistryData data({});
    data.families.push_back(family(MetricType::Counter, "requests_total", "Requests", 0));
    data.word_count = 1;
    data.snapshots = {{1}};

    fiber::mem::IoBuf out = fiber::mem::IoBuf::allocate(16);
    std::memcpy(out.writable_data(), "old", 3);
    out.commit(3);
    const std::size_t readable_before = out.readable();

    auto encoded = fiber::prometheus::detail::encode_text_into(data, out, {});
    ASSERT_FALSE(encoded);
    EXPECT_EQ(encoded.error(), IoErr::MessageTooLarge);
    EXPECT_EQ(out.readable(), readable_before);
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out.readable_data()), out.readable()), "old");
}

TEST(TextEncoderTest, ChainHonorsOutputLimitAndRejectsZeroChunk) {
    RegistryData data({});
    data.families.push_back(family(MetricType::Counter, "requests_total", "Requests", 0));
    data.word_count = 1;
    data.snapshots = {{1}};
    fiber::mem::IoBufNodePool pool;

    auto too_large = fiber::prometheus::detail::encode_text_chain(
            data, pool, CollectOptions{.chunk_size = 16, .max_output_bytes = 8});
    ASSERT_FALSE(too_large);
    EXPECT_EQ(too_large.error(), IoErr::MessageTooLarge);

    auto invalid = fiber::prometheus::detail::encode_text_chain(data, pool, CollectOptions{.chunk_size = 0});
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error(), IoErr::Invalid);
}

TEST(TextEncoderTest, EmptyRegistryProducesReadableEmptyChain) {
    RegistryData data({});
    fiber::mem::IoBufNodePool pool;
    auto encoded = fiber::prometheus::detail::encode_text_chain(data, pool, {});
    ASSERT_TRUE(encoded);
    EXPECT_TRUE(encoded->empty());
    EXPECT_TRUE(encoded->bound());
}

} // namespace
