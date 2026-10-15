#ifndef FIBER_PROMETHEUS_DYNAMIC_COUNTER_H
#define FIBER_PROMETHEUS_DYNAMIC_COUNTER_H

#include <cstdint>
#include <span>
#include <string_view>

#include <fiber/common/IoError.h>
#include "MetricFamily.h"

namespace fiber::prometheus {
namespace detail {
class DynamicSeriesStore;
}

// Copyable family handle. The Registry must outlive all handles and recording calls.
// Recording is synchronous and thread safe; no EventLoop is required.
class DynamicCounter {
public:
    [[nodiscard]] fiber::common::IoResult<void> inc(std::span<const std::string_view> labels = {}) const noexcept;
    [[nodiscard]] fiber::common::IoResult<void> add(std::span<const std::string_view> labels,
                                                    std::uint64_t delta) const noexcept;
    [[nodiscard]] DynamicSeriesDiagnostics diagnostics() const noexcept;

private:
    friend class MetricsRegistry;
    explicit DynamicCounter(detail::DynamicSeriesStore &store) noexcept : store_(&store) {}
    detail::DynamicSeriesStore *store_;
};

} // namespace fiber::prometheus

#endif // FIBER_PROMETHEUS_DYNAMIC_COUNTER_H
