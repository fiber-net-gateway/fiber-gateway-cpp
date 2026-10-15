#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <charconv>
#include <limits>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include <fiber/async/Spawn.h>
#include <fiber/event/EventLoop.h>
#include "DynamicSeriesStore.h"
#include "PrometheusInternal.h"
#include "fiber/prometheus/MetricsRegistry.h"

namespace {
using namespace fiber::prometheus;
using fiber::common::IoErr;
using fiber::common::IoResult;
constexpr std::array<std::string_view, 1> kNames{"user"};
constexpr SeriesOptions kTwo{.mode = SeriesMode::Lru, .max_series = 2};
std::array<std::string_view, 1> label(std::string_view value) { return {value}; }

class AllocationFailure {
public:
    explicit AllocationFailure(std::ptrdiff_t after) { detail::fail_dynamic_allocation_after(after); }
    ~AllocationFailure() { detail::fail_dynamic_allocation_after(-1); }
};

IoResult<std::string> collect(MetricsRegistry &registry, CollectOptions options = {}) {
    fiber::event::EventLoop loop;
    IoResult<std::string> result = std::unexpected(IoErr::Unknown);
    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        fiber::mem::IoBuf out = fiber::mem::IoBuf::allocate(65536);
        auto encoded = co_await registry.collect_text_into(out, options);
        if (encoded) {
            result = std::string(reinterpret_cast<const char *>(out.readable_data()), out.readable());
        } else {
            result = std::unexpected(encoded.error());
            EXPECT_EQ(out.readable(), 0);
        }
        loop.stop();
    });
    loop.run();
    return result;
}

TEST(DynamicSeriesTest, ValidatesConfigurationAndTypedBindings) {
    static_assert(!std::is_default_constructible_v<DynamicCounter>);
    static_assert(!std::is_default_constructible_v<DynamicHistogram>);
    MetricsRegistry registry;
    EXPECT_EQ(registry.register_counter("bad_total", "", {}, {.mode = SeriesMode::Lru, .max_series = 0}).error(),
              IoErr::Invalid);
    EXPECT_EQ(registry.register_counter("bad_total", "", {}, {.mode = SeriesMode::Static, .max_series = 1}).error(),
              IoErr::Invalid);
    EXPECT_EQ(registry.register_counter("bad_total", "", {},
                                        {.mode = SeriesMode::Static, .max_series = 0, .max_storage_bytes = 1})
                      .error(),
              IoErr::Invalid);
    EXPECT_EQ(registry.register_counter("bad_total", "", {}, {static_cast<SeriesMode>(99), 2}).error(), IoErr::Invalid);
    EXPECT_EQ(registry.register_counter("bad_total", "", kNames,
                                        {SeriesMode::Lru, std::numeric_limits<std::size_t>::max()})
                      .error(),
              IoErr::Invalid);
    MetricsRegistry overflow({.max_label_value_bytes = std::numeric_limits<std::size_t>::max()});
    EXPECT_EQ(overflow.register_counter("bad_total", "", kNames, kTwo).error(), IoErr::Invalid);
    MetricsRegistry zero_snapshot({.max_dynamic_snapshot_bytes = 0});
    EXPECT_EQ(zero_snapshot.register_counter("bad_total", "", {}, kTwo).error(), IoErr::Invalid);
    auto family = registry.register_counter("users_total", "Users", kNames, kTwo);
    auto histogram =
            registry.register_histogram("sizes", "", std::array<std::uint64_t, 1>{1}, HistogramUnit::Raw, kNames, kTwo);
    auto fixed = registry.register_counter("fixed_total", "");
    ASSERT_TRUE(family);
    ASSERT_TRUE(histogram);
    ASSERT_TRUE(fixed);
    EXPECT_EQ(registry.dynamic_counter(*family).error(), IoErr::Invalid);
    EXPECT_EQ(registry.register_series(*family, label("alice")).error(), IoErr::Invalid);
    ASSERT_TRUE(registry.freeze());
    EXPECT_TRUE(registry.dynamic_counter(*family));
    EXPECT_TRUE(registry.dynamic_histogram(*histogram));
    EXPECT_EQ(registry.dynamic_histogram(*family).error(), IoErr::Invalid);
    EXPECT_EQ(registry.dynamic_counter(*histogram).error(), IoErr::Invalid);
    EXPECT_EQ(registry.dynamic_counter(*fixed).error(), IoErr::Invalid);
    EXPECT_EQ(registry.dynamic_counter({}).error(), IoErr::Invalid);
    MetricsRegistry other;
    auto foreign = other.register_counter("users_total", "", kNames, kTwo);
    ASSERT_TRUE(foreign);
    EXPECT_EQ(registry.dynamic_counter(*foreign).error(), IoErr::Invalid);
    EXPECT_EQ(registry.register_counter("late_total", "", {}, kTwo).error(), IoErr::Invalid);
    EXPECT_EQ(registry.register_series(*family, label("alice")).error(), IoErr::Invalid);
}

TEST(DynamicSeriesTest, StrictLruResetsEvictedCounterAndCollectionDoesNotTouch) {
    MetricsRegistry registry;
    auto family = registry.register_counter("users_total", "Users", kNames, kTwo);
    ASSERT_TRUE(family);
    ASSERT_TRUE(registry.freeze());
    auto counter = *registry.dynamic_counter(*family);
    ASSERT_TRUE(collect(registry));
    EXPECT_TRUE(collect(registry)->empty());
    ASSERT_TRUE(counter.inc(label("alice")));
    ASSERT_TRUE(counter.inc(label("bob")));
    ASSERT_TRUE(counter.inc(label("alice")));
    const auto before = collect(registry);
    ASSERT_TRUE(before);
    EXPECT_NE(before->find("user=\"alice\"} 2\n"), std::string::npos);
    EXPECT_NE(before->find("user=\"bob\"} 1\n"), std::string::npos);
    ASSERT_TRUE(counter.inc(label("carol")));
    EXPECT_EQ(collect(registry)->find("bob"), std::string::npos);
    ASSERT_TRUE(counter.add(label("bob"), 0));
    auto text = collect(registry);
    ASSERT_TRUE(text);
    EXPECT_EQ(text->find("alice"), std::string::npos);
    EXPECT_NE(text->find("user=\"bob\"} 0\n"), std::string::npos);
    EXPECT_NE(text->find("user=\"carol\"} 1\n"), std::string::npos);
    EXPECT_LT(text->find("bob"), text->find("carol"));
    EXPECT_EQ(counter.diagnostics().active_series, 2);
    EXPECT_EQ(counter.diagnostics().evictions, 2);
}

TEST(DynamicSeriesTest, OwnsLabelsAndDistinguishesTuplesAndFullHashCollisions) {
    MetricsRegistry registry;
    auto family = registry.register_counter("tuples_total", "", std::array<std::string_view, 2>{"a", "b"},
                                            {.mode = SeriesMode::Lru, .max_series = 4});
    ASSERT_TRUE(family);
    ASSERT_TRUE(registry.freeze());
    auto counter = *registry.dynamic_counter(*family);
    detail::force_dynamic_hash_collisions(true);
    EXPECT_TRUE(counter.add(std::array<std::string_view, 2>{"ab", "c"}, 2));
    EXPECT_TRUE(counter.add(std::array<std::string_view, 2>{"a", "bc"}, 3));
    EXPECT_TRUE(counter.add(std::array<std::string_view, 2>{"", "abc"}, 4));
    std::string owned = "a|b";
    EXPECT_TRUE(counter.add(std::array<std::string_view, 2>{owned, ""}, 5));
    owned.assign("xxx");
    EXPECT_TRUE(counter.inc(std::array<std::string_view, 2>{"ab", "c"}));
    // Remove a hash-chain entry, then continue looking up colliding keys.
    EXPECT_TRUE(counter.inc(std::array<std::string_view, 2>{"new", "key"}));
    EXPECT_TRUE(counter.inc(std::array<std::string_view, 2>{"", "abc"}));
    detail::force_dynamic_hash_collisions(false);
    auto text = collect(registry);
    ASSERT_TRUE(text);
    EXPECT_NE(text->find("{a=\"ab\",b=\"c\"} 3\n"), std::string::npos);
    EXPECT_NE(text->find("{a=\"\",b=\"abc\"} 5\n"), std::string::npos);
    EXPECT_NE(text->find("{a=\"a|b\",b=\"\"} 5\n"), std::string::npos);
    EXPECT_EQ(text->find("xxx"), std::string::npos);
}

TEST(DynamicSeriesTest, ValidatesEscapedLengthsWithoutRefreshingLru) {
    MetricsRegistry registry({.max_label_value_bytes = 4});
    auto family = registry.register_counter("users_total", "", kNames, kTwo);
    ASSERT_TRUE(family);
    ASSERT_TRUE(registry.freeze());
    auto counter = *registry.dynamic_counter(*family);
    EXPECT_TRUE(counter.inc(label("a\"b"))); // Four bytes after escaping.
    EXPECT_TRUE(counter.inc(label("\\\n")));
    EXPECT_EQ(counter.inc(label("a\"bc")).error(), IoErr::Invalid);
    EXPECT_EQ(counter.inc({}).error(), IoErr::Invalid);
    EXPECT_TRUE(counter.inc(label("new")));
    auto text = collect(registry);
    ASSERT_TRUE(text);
    EXPECT_EQ(text->find("a\\\"b"), std::string::npos);
    EXPECT_NE(text->find("user=\"\\\\\\n\""), std::string::npos);
}

TEST(DynamicSeriesTest, SortsRawLabelBytesIncludingUnsignedAndPrefixes) {
    MetricsRegistry registry;
    auto family = registry.register_counter("users_total", "", kNames, {.mode = SeriesMode::Lru, .max_series = 6});
    ASSERT_TRUE(family);
    ASSERT_TRUE(registry.freeze());
    auto counter = *registry.dynamic_counter(*family);
    const std::string high(1, static_cast<char>(0xff));
    for (auto value: {std::string_view(high), std::string_view("ab"), std::string_view("a"), std::string_view("\""),
                      std::string_view("\\"), std::string_view("")}) {
        ASSERT_TRUE(counter.inc(label(value)));
    }
    auto text = collect(registry);
    ASSERT_TRUE(text);
    std::size_t position = 0;
    for (const auto &value:
         {std::string("user=\"\""), std::string(R"(user="\"")"), std::string(R"(user="\\")"), std::string("user=\"a\""),
          std::string("user=\"ab\""), std::string("user=\"") + high + "\""}) {
        const auto found = text->find(value, position);
        ASSERT_NE(found, std::string::npos);
        position = found + value.size();
    }
    EXPECT_EQ(text->back(), '\n');
}

TEST(DynamicSeriesTest, NoLabelFamilyUsesOneSlotAndFamiliesHaveIndependentCapacity) {
    MetricsRegistry registry;
    auto a = registry.register_counter("a_total", "", {}, kTwo);
    auto b = registry.register_counter("b_total", "", kNames, {.mode = SeriesMode::Lru, .max_series = 1});
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    ASSERT_TRUE(registry.freeze());
    auto first = *registry.dynamic_counter(*a);
    auto second = *registry.dynamic_counter(*b);
    EXPECT_TRUE(first.add({}, 0));
    EXPECT_TRUE(first.inc());
    EXPECT_TRUE(second.inc(label("one")));
    EXPECT_TRUE(second.add(label("two"), 3));
    EXPECT_EQ(first.diagnostics().active_series, 1);
    EXPECT_EQ(second.diagnostics().active_series, 1);
    EXPECT_EQ(second.diagnostics().evictions, 1);
    auto text = collect(registry);
    ASSERT_TRUE(text);
    EXPECT_NE(text->find("a_total 1\n"), std::string::npos);
    EXPECT_NE(text->find("b_total{user=\"two\"} 3\n"), std::string::npos);
    EXPECT_EQ(text->find("one"), std::string::npos);
}

TEST(DynamicSeriesTest, HistogramBoundariesUnitsAndRecreationResetAllFields) {
    MetricsRegistry registry;
    auto family = registry.register_histogram("latency_seconds", "Latency", std::array<std::uint64_t, 2>{1, 5},
                                              HistogramUnit::Microseconds, kNames,
                                              {.mode = SeriesMode::Lru, .max_series = 1});
    ASSERT_TRUE(family);
    ASSERT_TRUE(registry.freeze());
    auto histogram = *registry.dynamic_histogram(*family);
    for (std::uint64_t value: {0, 1, 2, 5, 6}) {
        ASSERT_TRUE(histogram.observe(label("alice"), value));
    }
    auto text = collect(registry);
    ASSERT_TRUE(text);
    EXPECT_NE(text->find("{user=\"alice\",le=\"0.000001\"} 2\n"), std::string::npos);
    EXPECT_NE(text->find("{user=\"alice\",le=\"0.000005\"} 4\n"), std::string::npos);
    EXPECT_NE(text->find("{user=\"alice\",le=\"+Inf\"} 5\n"), std::string::npos);
    EXPECT_NE(text->find("latency_seconds_sum{user=\"alice\"} 0.000014\n"), std::string::npos);
    EXPECT_TRUE(histogram.observe(label("bob"), 1));
    EXPECT_TRUE(histogram.observe(label("alice"), 9));
    text = collect(registry);
    ASSERT_TRUE(text);
    EXPECT_NE(text->find("{user=\"alice\",le=\"0.000005\"} 0\n"), std::string::npos);
    EXPECT_NE(text->find("{user=\"alice\",le=\"+Inf\"} 1\n"), std::string::npos);
    EXPECT_NE(text->find("latency_seconds_sum{user=\"alice\"} 0.000009\n"), std::string::npos);
    EXPECT_EQ(histogram.diagnostics().evictions, 2);
}

TEST(DynamicSeriesTest, FailedCreationPreservesValuesAndLruWhileHitsNeverAllocate) {
    MetricsRegistry registry;
    auto family = registry.register_counter("users_total", "", kNames, kTwo);
    ASSERT_TRUE(family);
    ASSERT_TRUE(registry.freeze());
    auto counter = *registry.dynamic_counter(*family);
    ASSERT_TRUE(counter.inc(label("alice")));
    ASSERT_TRUE(counter.inc(label("bob")));
    {
        AllocationFailure failure(0);
        EXPECT_EQ(counter.inc(label("carol")).error(), IoErr::NoMem);
        EXPECT_TRUE(counter.inc(label("bob")));
    }
    EXPECT_EQ(counter.diagnostics().creation_failures, 1);
    EXPECT_EQ(counter.diagnostics().evictions, 0);
    auto before = collect(registry);
    ASSERT_TRUE(before);
    EXPECT_NE(before->find("user=\"alice\"} 1\n"), std::string::npos);
    EXPECT_NE(before->find("user=\"bob\"} 2\n"), std::string::npos);
    ASSERT_TRUE(counter.inc(label("carol")));
    EXPECT_EQ(collect(registry)->find("alice"), std::string::npos);
}

TEST(DynamicSeriesTest, ExplicitStoreBudgetIncludesCandidateAndReclaimsOldLabels) {
    MetricsRegistry registry;
    auto family = registry.register_counter("users_total", "", kNames,
                                            {.mode = SeriesMode::Lru, .max_series = 2, .max_storage_bytes = 2048});
    ASSERT_TRUE(family);
    ASSERT_TRUE(registry.freeze());
    auto counter = *registry.dynamic_counter(*family);
    ASSERT_TRUE(counter.inc(label("alice")));
    ASSERT_TRUE(counter.inc(label("bob")));
    const auto baseline = counter.diagnostics().storage_bytes;
    EXPECT_EQ(counter.inc(label(std::string(1800, 'x'))).error(), IoErr::MessageTooLarge);
    EXPECT_EQ(counter.diagnostics().storage_bytes, baseline);
    EXPECT_EQ(counter.diagnostics().evictions, 0);
    ASSERT_TRUE(counter.inc(label("carol")));
    EXPECT_EQ(collect(registry)->find("alice"), std::string::npos);
    for (int i = 0; i < 2000; ++i) {
        auto value = std::to_string(i) + (i % 2 == 0 ? std::string(400, 'x') : "");
        ASSERT_TRUE(counter.inc(label(value)));
        EXPECT_LE(counter.diagnostics().storage_bytes, 2048);
    }
    ASSERT_TRUE(counter.inc(label("ccccc")));
    ASSERT_TRUE(counter.inc(label("ddd")));
    EXPECT_EQ(counter.diagnostics().storage_bytes, baseline);
}

TEST(DynamicSeriesTest, FreezeRollsBackEveryRuntimeAllocationAndCanBeRetried) {
    bool reached_success = false;
    for (std::ptrdiff_t after = 0; after < 20; ++after) {
        MetricsRegistry registry;
        auto a = registry.register_counter("a_total", "", kNames, kTwo);
        auto b = registry.register_counter("b_total", "", kNames, kTwo);
        fiber::event::EventLoop loop;
        auto fixed = registry.register_counter("fixed_total", "");
        ASSERT_TRUE(a);
        ASSERT_TRUE(b);
        ASSERT_TRUE(fixed);
        ASSERT_TRUE(registry.register_series(*fixed));
        ASSERT_TRUE(registry.add_shard(loop));
        IoResult<void> frozen;
        {
            AllocationFailure failure(after);
            frozen = registry.freeze();
        }
        if (frozen) {
            reached_success = true;
            break;
        }
        EXPECT_EQ(frozen.error(), IoErr::NoMem);
        EXPECT_FALSE(registry.frozen());
        EXPECT_EQ(registry.dynamic_counter(*a).error(), IoErr::Invalid);
        ASSERT_TRUE(registry.freeze());
        EXPECT_TRUE(registry.dynamic_counter(*a)->inc(label("a")));
        EXPECT_TRUE(registry.dynamic_counter(*b)->inc(label("b")));
    }
    EXPECT_TRUE(reached_success);
    MetricsRegistry too_small;
    ASSERT_TRUE(too_small.register_counter("a_total", "", kNames,
                                           {.mode = SeriesMode::Lru, .max_series = 2, .max_storage_bytes = 1}));
    EXPECT_EQ(too_small.freeze().error(), IoErr::MessageTooLarge);
    EXPECT_FALSE(too_small.frozen());
}

TEST(DynamicSeriesTest, ThreadsShareOneAggregateAndHistogramState) {
    MetricsRegistry registry;
    auto c = registry.register_counter("users_total", "", kNames, kTwo);
    auto h = registry.register_histogram("sizes", "", std::array<std::uint64_t, 2>{1, 5}, HistogramUnit::Raw, kNames,
                                         kTwo);
    ASSERT_TRUE(c);
    ASSERT_TRUE(h);
    ASSERT_TRUE(registry.freeze());
    auto counter = *registry.dynamic_counter(*c);
    auto histogram = *registry.dynamic_histogram(*h);
    std::atomic<bool> failed{false};
    std::vector<std::thread> threads;
    threads.reserve(8);
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&, counter, histogram]() {
            for (int j = 0; j < 2000; ++j) {
                if (!counter.inc(label("alice")) || !histogram.observe(label("alice"), 5)) {
                    failed.store(true);
                }
            }
        });
    }
    for (auto &thread: threads) {
        thread.join();
    }
    EXPECT_FALSE(failed.load());
    EXPECT_EQ(counter.diagnostics().active_series, 1);
    EXPECT_EQ(histogram.diagnostics().active_series, 1);
    auto text = collect(registry);
    ASSERT_TRUE(text);
    EXPECT_NE(text->find("users_total{user=\"alice\"} 16000\n"), std::string::npos);
    EXPECT_NE(text->find("sizes_bucket{user=\"alice\",le=\"5\"} 16000\n"), std::string::npos);
    EXPECT_NE(text->find("sizes_sum{user=\"alice\"} 80000\n"), std::string::npos);
    EXPECT_NE(text->find("sizes_count{user=\"alice\"} 16000\n"), std::string::npos);
}
TEST(DynamicSeriesTest, ConcurrentEvictionsKeepSnapshotLabelsAndValuesFromSameResidency) {
    detail::FamilySchema schema;
    schema.type = MetricType::Histogram;
    schema.options = {.mode = SeriesMode::Lru, .max_series = 8};
    schema.label_names = {"sequence"};
    schema.upper_bounds = {1'000'000};
    detail::DynamicSeriesStore store;
    ASSERT_TRUE(store.initialize(schema, 64));
    std::atomic<bool> stop{false};
    std::atomic<bool> failed{false};
    std::vector<std::thread> threads;
    threads.reserve(4);
    for (std::uint64_t worker = 0; worker < 4; ++worker) {
        threads.emplace_back([&, worker]() {
            std::uint64_t sequence = worker * 1'000'000;
            while (!stop.load(std::memory_order_relaxed)) {
                const auto value = std::to_string(++sequence);
                if (!store.record(label(value), sequence)) {
                    failed.store(true);
                }
            }
        });
    }
    detail::DynamicSnapshot snapshot;
    std::size_t bytes = 0;
    for (int round = 0; round < 500; ++round) {
        auto captured = store.capture(snapshot, bytes, 65536);
        EXPECT_TRUE(captured);
        EXPECT_LE(snapshot.samples().size(), 8);
        // Writers continue evicting/reusing all slots while the owned copy is read.
        for (const auto &sample: snapshot.samples()) {
            std::uint64_t sequence = 0;
            const auto value = sample.label_values[0];
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), sequence);
            EXPECT_EQ(parsed.ec, std::errc{});
            EXPECT_EQ(parsed.ptr, value.data() + value.size());
            EXPECT_EQ(sample.values[0], sequence <= 1'000'000 ? 1 : 0);
            EXPECT_EQ(sample.values[1], 1);
            EXPECT_EQ(sample.values[2], sequence);
        }
    }
    stop.store(true);
    for (auto &thread: threads) {
        thread.join();
    }
    EXPECT_FALSE(failed.load());
}

TEST(DynamicSeriesTest, SnapshotBudgetCoversAllFamiliesAndGrowthPeakAndRecovers) {
    MetricsRegistry registry({.max_dynamic_snapshot_bytes = 300});
    auto a = registry.register_counter("a_total", "", kNames, {.mode = SeriesMode::Lru, .max_series = 1});
    auto b = registry.register_counter("b_total", "", kNames, {.mode = SeriesMode::Lru, .max_series = 1});
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    ASSERT_TRUE(registry.freeze());
    auto first = *registry.dynamic_counter(*a);
    auto second = *registry.dynamic_counter(*b);
    EXPECT_TRUE(first.inc(label(std::string(80, 'a'))));
    EXPECT_TRUE(second.inc(label(std::string(80, 'b'))));
    auto failed = collect(registry);
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error(), IoErr::MessageTooLarge);
    EXPECT_TRUE(first.inc(label("a")));
    EXPECT_TRUE(second.inc(label("b")));
    auto recovered = collect(registry);
    ASSERT_TRUE(recovered);
    EXPECT_NE(recovered->find("a_total{user=\"a\"} 1\n"), std::string::npos);
    EXPECT_NE(recovered->find("b_total{user=\"b\"} 1\n"), std::string::npos);
}

} // namespace
