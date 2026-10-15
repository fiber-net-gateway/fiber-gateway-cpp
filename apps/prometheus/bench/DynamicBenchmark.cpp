#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include "fiber/prometheus/MetricsRegistry.h"

namespace {
using namespace fiber::prometheus;
using Clock = std::chrono::steady_clock;

struct Scenario {
    const char *name;
    std::size_t threads = 1;
    std::size_t capacity = 64;
    std::size_t label_bytes = 32;
    std::size_t buckets = 8;
    bool churn = false;
    bool separate_families = false;
    bool scrape = false;
    bool cold = false;
};

double nanoseconds(Clock::duration duration) noexcept {
    return std::chrono::duration<double, std::nano>(duration).count();
}

bool run(const Scenario &scenario, std::uint64_t iterations) {
    MetricsRegistry registry;
    std::vector<DynamicCounter> counters;
    std::vector<DynamicHistogram> histograms;
    const auto family_count = scenario.separate_families ? scenario.threads : 1;
    std::vector<std::uint64_t> bounds(scenario.buckets);
    for (std::size_t i = 0; i < bounds.size(); ++i) {
        bounds[i] = (i + 1) * 10;
    }
    constexpr std::array<std::string_view, 1> names{"key"};
    std::vector<FamilyId> counter_ids, histogram_ids;
    for (std::size_t i = 0; i < family_count; ++i) {
        const auto suffix = std::to_string(i);
        const SeriesOptions options{.mode = SeriesMode::Lru, .max_series = scenario.capacity};
        auto counter = registry.register_counter("requests_" + suffix + "_total", "Requests", names, options);
        auto histogram =
                registry.register_histogram("sizes_" + suffix, "Sizes", bounds, HistogramUnit::Raw, names, options);
        if (!counter || !histogram) {
            return false;
        }
        counter_ids.push_back(*counter);
        histogram_ids.push_back(*histogram);
    }
    if (!registry.freeze()) {
        return false;
    }
    const std::string stable(scenario.label_bytes, 's');
    const std::array<std::string_view, 1> stable_labels{stable};
    for (std::size_t i = 0; i < family_count; ++i) {
        counters.push_back(*registry.dynamic_counter(counter_ids[i]));
        histograms.push_back(*registry.dynamic_histogram(histogram_ids[i]));
        if (!scenario.cold &&
            (!counters.back().add(stable_labels, 0) || !histograms.back().observe(stable_labels, 0))) {
            return false;
        }
    }
    // All strings, result arrays, and latency samples are allocated before workers start.
    const std::size_t sample_count = static_cast<std::size_t>((iterations + 63) / 64);
    std::vector<std::vector<double>> latencies(scenario.threads, std::vector<double>(sample_count));
    std::atomic<bool> failed{false}, stop{false};
    std::atomic<std::uint64_t> scrapes{0};
    std::atomic<double> max_scrape_ns{0};
    std::thread scraper;
    if (scenario.scrape) {
        scraper = std::thread([&]() {
            fiber::event::EventLoop loop;
            fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
                while (!stop.load(std::memory_order_relaxed)) {
                    const auto start = Clock::now();
                    auto result = co_await registry.collect_text({.max_output_bytes = std::size_t{64} * 1024 * 1024});
                    if (!result && !failed.exchange(true)) {
                        const auto error = fiber::common::io_err_name(result.error());
                        std::fprintf(stderr, "scrape failed: %.*s\n", static_cast<int>(error.size()), error.data());
                    }
                    const double elapsed = nanoseconds(Clock::now() - start);
                    max_scrape_ns.store(std::max(max_scrape_ns.load(), elapsed));
                    scrapes.fetch_add(1);
                    co_await fiber::async::sleep(std::chrono::milliseconds(1));
                }
                loop.stop();
            });
            loop.run();
        });
    }
    std::barrier start_line(static_cast<std::ptrdiff_t>(scenario.threads + 1));
    std::vector<std::thread> workers;
    workers.reserve(scenario.threads);
    for (std::size_t worker = 0; worker < scenario.threads; ++worker) {
        workers.emplace_back([&, worker]() {
            const auto index = scenario.separate_families ? worker : 0;
            auto counter = counters[index];
            auto histogram = histograms[index];
            std::array<char, 4096> text{};
            std::fill_n(text.data(), scenario.label_bytes, 'x');
            std::array<std::string_view, 1> labels{stable};
            start_line.arrive_and_wait();
            for (std::uint64_t i = 0; i < iterations; ++i) {
                if (scenario.churn || scenario.cold) {
                    // Include request label generation in the churn cost; no heap allocation.
                    std::fill_n(text.data(), 32, 'x');
                    const auto id = (i * scenario.threads) + worker;
                    (void) std::to_chars(text.data(), text.data() + 32, id);
                    labels[0] = std::string_view(text.data(), scenario.label_bytes);
                }
                const bool sampled = (i % 64) == 0;
                const auto start = sampled ? Clock::now() : Clock::time_point{};
                if (!counter.inc(labels) || !histogram.observe(labels, i % 2000)) {
                    failed.store(true);
                }
                if (sampled) {
                    latencies[worker][i / 64] = nanoseconds(Clock::now() - start);
                }
            }
        });
    }
    const auto started = Clock::now();
    start_line.arrive_and_wait();
    for (auto &worker: workers) {
        worker.join();
    }
    const auto elapsed = Clock::now() - started;
    stop.store(true);
    if (scraper.joinable()) {
        scraper.join();
    }
    std::vector<double> sorted;
    std::size_t storage = 0;
    std::uint64_t label_allocations = 0;
    for (std::size_t i = 0; i < family_count; ++i) {
        for (const auto stats: {counters[i].diagnostics(), histograms[i].diagnostics()}) {
            storage += stats.storage_bytes;
            label_allocations += stats.evictions + stats.active_series - (scenario.cold ? 0 : 1);
        }
    }
    for (const auto &samples: latencies) {
        sorted.insert(sorted.end(), samples.begin(), samples.end());
    }
    std::sort(sorted.begin(), sorted.end());
    const auto percentile = [&](std::size_t percent) { return sorted[(sorted.size() - 1) * percent / 100]; };
    const std::uint64_t pairs = iterations * scenario.threads;
    std::printf("scenario=%s threads=%zu N=%zu label_bytes=%zu buckets=%zu scrape_interval_ms=%d "
                "pairs=%llu pairs_per_second=%.0f sampled_p95_ns=%.0f sampled_p99_ns=%.0f "
                "record_label_allocations=%llu store_bytes=%zu scrapes=%llu max_scrape_us=%.0f\n",
                scenario.name, scenario.threads, scenario.capacity, scenario.label_bytes, scenario.buckets,
                scenario.scrape ? 1 : 0, static_cast<unsigned long long>(pairs),
                static_cast<double>(iterations * scenario.threads) / std::chrono::duration<double>(elapsed).count(),
                percentile(95), percentile(99), static_cast<unsigned long long>(label_allocations), storage,
                static_cast<unsigned long long>(scrapes.load()), max_scrape_ns.load() / 1000);
    return !failed.load();
}
} // namespace

int main(int argc, char **argv) {
    std::uint64_t iterations = 100'000;
    if (argc == 2) {
        const std::string_view input(argv[1]);
        const auto parsed = std::from_chars(input.data(), input.data() + input.size(), iterations);
        if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size() || iterations == 0 ||
            iterations > 100'000'000) {
            return 2;
        }
    }
    for (std::size_t threads: {1, 2, 4, 8}) {
        if (!run({.name = "steady_shared", .threads = threads}, iterations) ||
            !run({.name = "steady_separate", .threads = threads, .separate_families = true}, iterations)) {
            return 1;
        }
    }
    if (!run({.name = "cold", .capacity = 1024, .cold = true}, 1024) ||
        !run({.name = "churn", .churn = true}, iterations) ||
        !run({.name = "churn_scrape", .threads = 4, .churn = true, .scrape = true}, iterations) ||
        !run({.name = "large_labels_buckets_scrape",
              .threads = 4,
              .label_bytes = 4096,
              .buckets = 128,
              .churn = true,
              .scrape = true},
             (iterations / 10) + 1)) {
        return 1;
    }
    return 0;
}
