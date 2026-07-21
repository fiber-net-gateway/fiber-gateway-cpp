#ifndef FIBER_PROMETHEUS_INTERNAL_H
#define FIBER_PROMETHEUS_INTERNAL_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "async/WaitGroup.h"
#include "event/EventLoop.h"
#include "fiber/prometheus/MetricFamily.h"
#include "fiber/prometheus/MetricsShard.h"

namespace fiber::prometheus {

class MetricsRegistry;
class MetricsShard;

namespace detail {

inline constexpr std::size_t kCacheLineSize = 64;

struct SeriesSchema {
    std::vector<std::string> label_values;
    std::size_t word_offset = 0;
};

struct FamilySchema {
    MetricType type = MetricType::Counter;
    GaugeReduction reduction = GaugeReduction::Sum;
    HistogramUnit histogram_unit = HistogramUnit::Raw;
    std::string name;
    std::string help;
    std::vector<std::string> label_names;
    std::vector<std::uint64_t> upper_bounds;
    std::vector<SeriesSchema> series;
};

struct ShardData {
    MetricsRegistry *registry = nullptr;
    fiber::event::EventLoop *owner = nullptr;
    std::byte *storage = nullptr;
    std::size_t storage_bytes = 0;

    ShardData(MetricsRegistry &registry, fiber::event::EventLoop &owner) noexcept :
        registry(&registry), owner(&owner) {}
    ~ShardData();

    [[nodiscard]] bool allocate(std::size_t word_count) noexcept;
    void reset() noexcept;
    [[nodiscard]] std::uint64_t *words() noexcept { return reinterpret_cast<std::uint64_t *>(storage); }
};

struct SnapshotRequest {
    MetricsRegistry *registry = nullptr;
    std::size_t shard_index = 0;
    fiber::event::EventLoop::NotifyEntry notify_entry{};
};

struct RegistryData {
    explicit RegistryData(RegistryOptions options) : options(options) {}

    RegistryOptions options;
    std::vector<FamilySchema> families;
    std::vector<std::unique_ptr<MetricsShard>> shards;
    std::vector<std::vector<std::uint64_t>> snapshots;
    std::vector<std::unique_ptr<SnapshotRequest>> snapshot_requests;
    std::vector<std::uint64_t> histogram_scratch;
    fiber::async::WaitGroup snapshot_wait;
    fiber::async::WaitGroup idle_wait;
    std::mutex collect_mutex;
    std::size_t pending_snapshots = 0;
    std::size_t word_count = 0;
    bool frozen = false;
    bool accepting_collects = true;
    bool collect_active = false;
    bool collect_attached = false;
};

} // namespace detail
} // namespace fiber::prometheus

#endif // FIBER_PROMETHEUS_INTERNAL_H
