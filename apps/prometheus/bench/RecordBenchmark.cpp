#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include "fiber/prometheus/Counter.h"
#include "fiber/prometheus/Histogram.h"

namespace {

std::uint64_t parse_iterations(int argc, char **argv) noexcept {
    constexpr std::uint64_t default_iterations = 100'000'000;
    if (argc != 2) {
        return default_iterations;
    }
    std::uint64_t value = 0;
    const std::string_view input(argv[1]);
    auto [end, error] = std::from_chars(input.data(), input.data() + input.size(), value);
    return error == std::errc{} && end == input.data() + input.size() && value != 0 ? value : default_iterations;
}

template<typename Value>
inline void keep_observable(Value &value) noexcept {
#if defined(__clang__) || defined(__GNUC__)
    asm volatile("" : "+m"(value) : : "memory");
#else
    (void) value;
#endif
}

double operations_per_second(std::uint64_t operations, std::chrono::steady_clock::duration elapsed) noexcept {
    return static_cast<double>(operations) / std::chrono::duration_cast<std::chrono::duration<double>>(elapsed).count();
}

} // namespace

int main(int argc, char **argv) {
    const std::uint64_t iterations = parse_iterations(argc, argv);

    std::uint64_t counter_value = 0;
    fiber::prometheus::CounterRef counter(counter_value);
    const auto counter_start = std::chrono::steady_clock::now();
    for (std::uint64_t index = 0; index < iterations; ++index) {
        counter.inc();
        keep_observable(counter_value);
    }
    const auto counter_elapsed = std::chrono::steady_clock::now() - counter_start;

    constexpr std::array<std::uint64_t, 8> bounds{1, 5, 10, 50, 100, 500, 1'000, 10'000};
    std::array<std::uint64_t, bounds.size()> intervals{};
    std::uint64_t histogram_count = 0;
    std::uint64_t histogram_sum = 0;
    fiber::prometheus::HistogramRef histogram(bounds, intervals, histogram_count, histogram_sum);
    const auto histogram_start = std::chrono::steady_clock::now();
    for (std::uint64_t index = 0; index < iterations; ++index) {
        histogram.observe(index % 20'000);
        keep_observable(histogram_count);
    }
    const auto histogram_elapsed = std::chrono::steady_clock::now() - histogram_start;

    std::printf("iterations=%llu\n", static_cast<unsigned long long>(iterations));
    std::printf("counter_ops_per_second=%.0f final=%llu\n", operations_per_second(iterations, counter_elapsed),
                static_cast<unsigned long long>(counter_value));
    std::printf("histogram_ops_per_second=%.0f count=%llu sum=%llu\n",
                operations_per_second(iterations, histogram_elapsed), static_cast<unsigned long long>(histogram_count),
                static_cast<unsigned long long>(histogram_sum));
    return 0;
}
