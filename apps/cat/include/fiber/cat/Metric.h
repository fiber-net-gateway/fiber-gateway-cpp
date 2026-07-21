#ifndef FIBER_CAT_METRIC_H
#define FIBER_CAT_METRIC_H

#include <chrono>
#include <cstdint>
#include <expected>
#include <string_view>

#include "Message.h"

namespace fiber::cat {

namespace detail {
struct MetricData;
}

enum class MetricKind : std::uint8_t {
    Count,
    Duration,
};

struct MetricSnapshot {
    std::string_view name;
    MetricKind kind = MetricKind::Count;
    std::int64_t quantity = 0;
    std::uint64_t duration_sum_millis = 0;
};

class Metric {
public:
    Metric() noexcept = default;
    Metric(const Metric &) = delete;
    Metric &operator=(const Metric &) = delete;
    Metric(Metric &&other) noexcept;
    Metric &operator=(Metric &&other) noexcept;
    ~Metric();

    [[nodiscard]] static std::expected<Metric, RecordError> create_count(std::string_view name) noexcept;
    [[nodiscard]] static std::expected<Metric, RecordError> create_duration(std::string_view name) noexcept;

    [[nodiscard]] bool valid() const noexcept { return data_ != nullptr; }
    [[nodiscard]] std::string_view name() const noexcept;
    [[nodiscard]] MetricKind kind() const noexcept;

    RecordError record_count(std::int64_t quantity = 1) noexcept;
    RecordError record_duration(std::chrono::milliseconds duration) noexcept;

    [[nodiscard]] std::expected<MetricSnapshot, RecordError> snapshot() const noexcept;
    [[nodiscard]] std::expected<MetricSnapshot, RecordError> snapshot_and_reset() noexcept;
    RecordError reset() noexcept;

private:
    explicit Metric(detail::MetricData *data) noexcept : data_(data) {}

    [[nodiscard]] static std::expected<Metric, RecordError> create(MetricKind kind, std::string_view name) noexcept;

    detail::MetricData *data_ = nullptr;
};

} // namespace fiber::cat

#endif // FIBER_CAT_METRIC_H
