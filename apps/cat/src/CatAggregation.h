#ifndef FIBER_CAT_AGGREGATION_H
#define FIBER_CAT_AGGREGATION_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include <common/mem/BufPool.h>
#include <event/EventLoop.h>
#include <fiber/cat/Message.h>
#include <fiber/cat/Metric.h>

namespace fiber::cat::detail {

class CatClientCore;
struct MessageData;
struct MessageTraceData;

enum class AggregateKind : std::uint8_t {
    Transaction,
    Event,
    MetricCount,
    MetricDuration,
};

struct AggregateValue {
    AggregateKind kind = AggregateKind::Event;
    std::string_view type;
    std::string_view name;
    std::uint64_t count = 0;
    std::uint64_t error_count = 0;
    std::uint64_t duration_sum_millis = 0;
};

class AggregationShard {
public:
    using Visitor = bool (*)(void *, const AggregateValue &) noexcept;

    [[nodiscard]] static AggregationShard *create(event::EventLoop &owner, std::size_t max_keys,
                                                  std::size_t max_key_bytes, std::size_t max_bytes,
                                                  std::size_t max_duration_buckets) noexcept;
    ~AggregationShard();

    AggregationShard(const AggregationShard &) = delete;
    AggregationShard &operator=(const AggregationShard &) = delete;

    [[nodiscard]] event::EventLoop &owner() const noexcept { return *owner_; }
    [[nodiscard]] std::size_t aggregate(const MessageTraceData &trace) noexcept;
    [[nodiscard]] std::expected<void *, RecordError> register_metric(MetricKind kind, std::string_view name) noexcept;
    RecordError record_metric_count(void *handle, std::int64_t quantity) noexcept;
    RecordError record_metric_duration(void *handle, std::chrono::milliseconds duration) noexcept;
    void request_flush(std::shared_ptr<CatClientCore> core) noexcept;
    void flush(CatClientCore &core) noexcept;
    void discard_pending(CatClientCore &core) noexcept;

    void for_each(void *opaque, Visitor visitor) const noexcept;
    [[nodiscard]] std::size_t key_count() const noexcept { return key_count_; }

private:
    struct DurationBucket;
    struct Entry;
    struct FlushRequest;

    AggregationShard(event::EventLoop &owner, std::size_t max_keys, std::size_t max_key_bytes, std::size_t max_bytes,
                     std::size_t max_duration_buckets, Entry **buckets, std::size_t bucket_count) noexcept;

    [[nodiscard]] std::size_t aggregate_message(const MessageData &message) noexcept;
    [[nodiscard]] Entry *find_or_create(AggregateKind kind, std::string_view type, std::string_view name,
                                        std::size_t &dropped) noexcept;
    [[nodiscard]] bool add_duration(Entry &entry, std::uint64_t duration_millis) noexcept;
    [[nodiscard]] bool flush_kind(CatClientCore &core, AggregateKind kind) noexcept;
    [[nodiscard]] bool flush_metrics(CatClientCore &core) noexcept;
    void reset_kind(AggregateKind kind) noexcept;

    static void on_flush_notify(FlushRequest *request) noexcept;

    event::EventLoop *owner_ = nullptr;
    mem::BufPool pool_;
    Entry **buckets_ = nullptr;
    std::size_t bucket_count_ = 0;
    Entry *all_head_ = nullptr;
    Entry *all_tail_ = nullptr;
    std::size_t max_keys_ = 0;
    std::size_t max_key_bytes_ = 0;
    std::size_t max_bytes_ = 0;
    std::size_t max_duration_buckets_ = 0;
    std::size_t key_count_ = 0;
    std::size_t allocated_bytes_ = 0;
};

} // namespace fiber::cat::detail

#endif // FIBER_CAT_AGGREGATION_H
