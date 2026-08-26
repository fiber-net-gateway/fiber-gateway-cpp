#ifndef FIBER_PROMETHEUS_METRICS_REGISTRY_H
#define FIBER_PROMETHEUS_METRICS_REGISTRY_H

#include <memory>
#include <span>
#include <string_view>

#include <fiber/async/Task.h>
#include <fiber/common/IoError.h>
#include <fiber/common/mem/IoBuf.h>
#include <fiber/common/mem/IoBufChain.h>
#include "MetricFamily.h"
#include "MetricsShard.h"

namespace fiber::event {
class EventLoop;
}

namespace fiber::prometheus {

namespace detail {
struct RegistryData;
struct SnapshotRequest;
} // namespace detail

struct CollectOptions {
    std::size_t chunk_size = 4 * 1024;
    std::size_t max_output_bytes = 16 * 1024 * 1024;
};

class MetricsRegistry {
public:
    explicit MetricsRegistry(RegistryOptions options = {});
    ~MetricsRegistry();

    MetricsRegistry(const MetricsRegistry &) = delete;
    MetricsRegistry &operator=(const MetricsRegistry &) = delete;
    MetricsRegistry(MetricsRegistry &&) = delete;
    MetricsRegistry &operator=(MetricsRegistry &&) = delete;

    [[nodiscard]] fiber::common::IoResult<FamilyId>
    register_counter(std::string_view name, std::string_view help, std::span<const std::string_view> label_names = {});

    [[nodiscard]] fiber::common::IoResult<FamilyId> register_gauge(std::string_view name, std::string_view help,
                                                                   GaugeReduction reduction,
                                                                   std::span<const std::string_view> label_names = {});

    [[nodiscard]] fiber::common::IoResult<FamilyId>
    register_histogram(std::string_view name, std::string_view help, std::span<const std::uint64_t> upper_bounds,
                       HistogramUnit unit = HistogramUnit::Raw, std::span<const std::string_view> label_names = {});

    [[nodiscard]] fiber::common::IoResult<SeriesId>
    register_series(FamilyId family, std::span<const std::string_view> label_values = {});

    [[nodiscard]] fiber::common::IoResult<ShardId> add_shard(fiber::event::EventLoop &owner);
    [[nodiscard]] fiber::common::IoResult<void> freeze();

    [[nodiscard]] bool frozen() const noexcept;
    [[nodiscard]] std::size_t family_count() const noexcept;
    [[nodiscard]] std::size_t shard_count() const noexcept;
    [[nodiscard]] MetricsShard *shard(ShardId id) noexcept;
    [[nodiscard]] const MetricsShard *shard(ShardId id) const noexcept;

    // Overlapping collections share one stable shard snapshot generation and encode into caller-owned output.
    [[nodiscard]] fiber::async::Task<fiber::common::IoResult<fiber::mem::IoBufChain>>
    collect_text(fiber::mem::IoBufNodePool &node_pool, CollectOptions options = {}) noexcept;

    [[nodiscard]] fiber::async::Task<fiber::common::IoResult<std::size_t>>
    collect_text_into(fiber::mem::IoBuf &out, CollectOptions options = {}) noexcept;

    void stop_collecting() noexcept;
    [[nodiscard]] fiber::async::Task<void> wait_for_idle() noexcept;

private:
    friend class MetricsShard;

    class CollectToken {
    public:
        CollectToken(const CollectToken &) = delete;
        CollectToken &operator=(const CollectToken &) = delete;
        CollectToken(CollectToken &&other) noexcept;
        CollectToken &operator=(CollectToken &&other) noexcept;
        ~CollectToken();

    private:
        friend class MetricsRegistry;

        explicit CollectToken(MetricsRegistry &registry) noexcept : registry_(&registry) {}
        MetricsRegistry *registry_ = nullptr;
    };

    [[nodiscard]] fiber::common::IoResult<FamilyId>
    register_family(MetricType type, std::string_view name, std::string_view help,
                    std::span<const std::string_view> label_names, GaugeReduction reduction,
                    std::span<const std::uint64_t> upper_bounds, HistogramUnit unit);

    [[nodiscard]] fiber::async::Task<fiber::common::IoResult<CollectToken>> prepare_collect() noexcept;
    void release_collect() noexcept;
    void copy_snapshot(std::size_t shard_index) noexcept;
    void snapshot_finished() noexcept;
    static void run_snapshot(detail::SnapshotRequest *request) noexcept;

    std::unique_ptr<detail::RegistryData> data_;
};

} // namespace fiber::prometheus

#endif // FIBER_PROMETHEUS_METRICS_REGISTRY_H
