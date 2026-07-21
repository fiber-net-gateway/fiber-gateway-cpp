#ifndef FIBER_PROMETHEUS_METRICS_SHARD_H
#define FIBER_PROMETHEUS_METRICS_SHARD_H

#include <memory>

#include "Counter.h"
#include "Gauge.h"
#include "Histogram.h"
#include "MetricFamily.h"
#include "common/IoError.h"

namespace fiber::event {
class EventLoop;
}

namespace fiber::prometheus {

namespace detail {
struct ShardData;
}

class MetricsRegistry;

class MetricsShard {
public:
    ~MetricsShard();

    MetricsShard(const MetricsShard &) = delete;
    MetricsShard &operator=(const MetricsShard &) = delete;
    MetricsShard(MetricsShard &&) = delete;
    MetricsShard &operator=(MetricsShard &&) = delete;

    [[nodiscard]] fiber::event::EventLoop &owner_loop() const noexcept;

    [[nodiscard]] fiber::common::IoResult<CounterRef> counter(SeriesId series) noexcept;
    [[nodiscard]] fiber::common::IoResult<GaugeRef> gauge(SeriesId series) noexcept;
    [[nodiscard]] fiber::common::IoResult<HistogramRef> histogram(SeriesId series) noexcept;

private:
    friend class MetricsRegistry;

    MetricsShard(MetricsRegistry &registry, fiber::event::EventLoop &owner);

    std::unique_ptr<detail::ShardData> data_;
};

} // namespace fiber::prometheus

#endif // FIBER_PROMETHEUS_METRICS_SHARD_H
