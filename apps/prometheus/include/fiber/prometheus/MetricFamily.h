#ifndef FIBER_PROMETHEUS_METRIC_FAMILY_H
#define FIBER_PROMETHEUS_METRIC_FAMILY_H

#include <cstddef>
#include <cstdint>

namespace fiber::prometheus {

class MetricsRegistry;
class MetricsShard;

enum class MetricType : std::uint8_t { Counter, Gauge, Histogram };

enum class GaugeReduction : std::uint8_t { Sum, Min, Max };

enum class HistogramUnit : std::uint8_t { Raw, Nanoseconds, Microseconds, Milliseconds, Seconds };

struct RegistryOptions {
    std::size_t max_help_bytes = 64 * 1024;
    std::size_t max_label_value_bytes = 4 * 1024;
};

class FamilyId {
public:
    FamilyId() noexcept = default;
    [[nodiscard]] bool valid() const noexcept { return registry_ != nullptr; }

private:
    friend class MetricsRegistry;

    FamilyId(const MetricsRegistry *registry, std::uint32_t index) noexcept : registry_(registry), index_(index) {}

    const MetricsRegistry *registry_ = nullptr;
    std::uint32_t index_ = 0;
};

class SeriesId {
public:
    SeriesId() noexcept = default;
    [[nodiscard]] bool valid() const noexcept { return registry_ != nullptr; }

private:
    friend class MetricsRegistry;
    friend class MetricsShard;

    SeriesId(const MetricsRegistry *registry, std::uint32_t family_index, std::uint32_t series_index) noexcept :
        registry_(registry), family_index_(family_index), series_index_(series_index) {}

    const MetricsRegistry *registry_ = nullptr;
    std::uint32_t family_index_ = 0;
    std::uint32_t series_index_ = 0;
};

class ShardId {
public:
    ShardId() noexcept = default;
    [[nodiscard]] bool valid() const noexcept { return registry_ != nullptr; }

private:
    friend class MetricsRegistry;

    ShardId(const MetricsRegistry *registry, std::uint32_t index) noexcept : registry_(registry), index_(index) {}

    const MetricsRegistry *registry_ = nullptr;
    std::uint32_t index_ = 0;
};

} // namespace fiber::prometheus

#endif // FIBER_PROMETHEUS_METRIC_FAMILY_H
