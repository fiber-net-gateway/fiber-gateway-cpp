#ifndef FIBER_PROMETHEUS_DYNAMIC_SERIES_STORE_H
#define FIBER_PROMETHEUS_DYNAMIC_SERIES_STORE_H

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string_view>

#include <fiber/common/IoError.h>
#include "fiber/prometheus/MetricFamily.h"

namespace fiber::prometheus::detail {

struct FamilySchema;

// Private, thread-local allocation failure injection. -1 disables it; 0 fails
// the next allocation. It never changes another recording thread's allocator.
void force_dynamic_hash_collisions(bool enabled) noexcept;
void fail_dynamic_allocation_after(std::ptrdiff_t successful_allocations) noexcept;
void *dynamic_allocate(std::size_t bytes) noexcept;
void dynamic_deallocate(void *memory) noexcept;
std::size_t dynamic_allocation_bytes(const void *memory) noexcept;

struct DynamicSample {
    std::span<const std::string_view> label_values;
    const std::uint64_t *values = nullptr;
};

class DynamicSnapshot {
public:
    ~DynamicSnapshot();
    DynamicSnapshot() noexcept = default;
    DynamicSnapshot(const DynamicSnapshot &) = delete;
    DynamicSnapshot &operator=(const DynamicSnapshot &) = delete;

    [[nodiscard]] std::span<const DynamicSample> samples() const noexcept { return {samples_, size_}; }
    [[nodiscard]] std::size_t storage_bytes() const noexcept { return dynamic_allocation_bytes(memory_); }
    void sort() noexcept;

private:
    friend class DynamicSeriesStore;
    fiber::common::IoResult<void> reserve(std::size_t bytes, std::size_t &total, std::size_t budget) noexcept;
    void *memory_ = nullptr;
    std::size_t capacity_bytes_ = 0;
    DynamicSample *samples_ = nullptr;
    std::size_t size_ = 0;
};

class DynamicSeriesStore {
public:
    DynamicSeriesStore() noexcept = default;
    ~DynamicSeriesStore();
    DynamicSeriesStore(const DynamicSeriesStore &) = delete;
    DynamicSeriesStore &operator=(const DynamicSeriesStore &) = delete;

    [[nodiscard]] static bool valid_layout(SeriesOptions options, std::size_t labels, std::size_t label_limit,
                                           std::size_t words) noexcept;
    [[nodiscard]] fiber::common::IoResult<void> initialize(const FamilySchema &schema,
                                                           std::size_t label_limit) noexcept;
    [[nodiscard]] fiber::common::IoResult<void> record(std::span<const std::string_view> labels,
                                                       std::uint64_t value) noexcept;
    [[nodiscard]] fiber::common::IoResult<void> capture(DynamicSnapshot &snapshot, std::size_t &total,
                                                        std::size_t budget) noexcept;
    [[nodiscard]] DynamicSeriesDiagnostics diagnostics() const noexcept;

private:
    static constexpr std::size_t kNone = static_cast<std::size_t>(-1);
    struct Slot {
        void *labels = nullptr;
        std::uint64_t hash = 0;
        std::size_t previous = kNone;
        std::size_t next = kNone;
        std::size_t hash_next = kNone;
    };

    void touch(std::size_t index) noexcept;
    void unlink(std::size_t index) noexcept;
    [[nodiscard]] bool matches(const Slot &slot, std::span<const std::string_view> labels) const noexcept;
    [[nodiscard]] std::size_t snapshot_fixed_bytes() const noexcept;

    const FamilySchema *schema_ = nullptr;
    mutable std::mutex mutex_;
    void *fixed_ = nullptr;
    Slot *slots_ = nullptr;
    std::size_t *index_ = nullptr;
    std::uint64_t *values_ = nullptr;
    std::size_t label_limit_ = 0;
    std::size_t words_ = 0;
    std::size_t head_ = kNone;
    std::size_t tail_ = kNone;
    DynamicSeriesDiagnostics stats_;
};

struct DynamicFamily {
    DynamicSeriesStore store;
    DynamicSnapshot snapshot;
};

} // namespace fiber::prometheus::detail
#endif // FIBER_PROMETHEUS_DYNAMIC_SERIES_STORE_H
