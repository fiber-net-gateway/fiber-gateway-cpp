#ifndef FIBER_PROMETHEUS_INTERNAL_H
#define FIBER_PROMETHEUS_INTERNAL_H

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <fiber/async/WaitGroup.h>
#include <fiber/common/Assert.h>
#include <fiber/event/EventLoop.h>
#include "DynamicSeriesStore.h"
#include "fiber/prometheus/MetricFamily.h"
#include "fiber/prometheus/MetricsShard.h"

namespace fiber::prometheus {

class MetricsRegistry;
class MetricsShard;

namespace detail {

inline constexpr std::size_t kCacheLineSize = 64;

// Runtime arrays never grow and report allocation failure without exceptions.
template<typename T>
class RuntimeArray {
public:
    RuntimeArray() noexcept = default;
    RuntimeArray(const RuntimeArray &) = delete;
    RuntimeArray &operator=(const RuntimeArray &) = delete;
    ~RuntimeArray() { reset(); }
    bool allocate(std::size_t size) noexcept {
        reset();
        if (size == 0) {
            return true;
        }
        if (size > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            return false;
        }
        data_ = static_cast<T *>(dynamic_allocate(size * sizeof(T)));
        if (!data_) {
            return false;
        }
        size_ = size;
        for (std::size_t i = 0; i < size_; ++i) {
            std::construct_at(data_ + i);
        }
        return true;
    }
    void reset() noexcept {
        for (std::size_t i = 0; i < size_; ++i) {
            std::destroy_at(data_ + i);
        }
        dynamic_deallocate(data_);
        data_ = nullptr;
        size_ = 0;
    }
    [[nodiscard]] T *data() noexcept { return data_; }
    [[nodiscard]] const T *data() const noexcept { return data_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] T *begin() noexcept { return data_; }
    [[nodiscard]] T *end() noexcept { return size_ == 0 ? data_ : data_ + size_; }
    [[nodiscard]] const T *begin() const noexcept { return data_; }
    [[nodiscard]] const T *end() const noexcept { return size_ == 0 ? data_ : data_ + size_; }
    T &operator[](std::size_t i) noexcept { return data_[i]; }
    const T &operator[](std::size_t i) const noexcept { return data_[i]; }

private:
    T *data_ = nullptr;
    std::size_t size_ = 0;
};

struct SeriesSchema {
    std::vector<std::string> label_values;
    std::size_t word_offset = 0;
};

struct FamilySchema {
    MetricType type = MetricType::Counter;
    SeriesOptions options;
    std::size_t dynamic_index = 0;
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
    // Const-ness deliberately does not propagate through the interior
    // pointer: metric counters are written through words() on live shards.
    [[nodiscard]] std::uint64_t *words() const noexcept { return reinterpret_cast<std::uint64_t *>(storage); }
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
    RuntimeArray<RuntimeArray<std::uint64_t>> snapshots;
    RuntimeArray<SnapshotRequest> snapshot_requests;
    RuntimeArray<DynamicFamily> dynamic_families;
    std::size_t dynamic_snapshot_bytes = 0;
    fiber::common::IoErr snapshot_error = fiber::common::IoErr::None;
    fiber::async::WaitGroup snapshot_wait;
    fiber::async::WaitGroup idle_wait;
    std::mutex collect_mutex;
    std::size_t pending_snapshots = 0;
    std::size_t attached_collectors = 0;
    std::size_t word_count = 0;
    bool frozen = false;
    bool accepting_collects = true;
    bool collect_active = false;
};

} // namespace detail
} // namespace fiber::prometheus

#endif // FIBER_PROMETHEUS_INTERNAL_H
