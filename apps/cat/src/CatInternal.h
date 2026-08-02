#ifndef FIBER_CAT_INTERNAL_H
#define FIBER_CAT_INTERNAL_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string_view>

#include <common/mem/BufPool.h>
#include <event/EventLoop.h>
#include <fiber/cat/MessageTrace.h>
#include <fiber/cat/Status.h>

namespace fiber::cat {
class Event;
class Transaction;
} // namespace fiber::cat

namespace fiber::cat::detail {

inline constexpr std::size_t kChildrenPerChunk = 16;

class CatClientCore;
class AggregationShard;
struct MessageTrace;

struct TraceContext {
    std::shared_ptr<CatClientCore> core;
    AggregationShard *aggregation_shard = nullptr;
    MessageTraceContext propagation_context;
};

[[nodiscard]] RecordError validate_trace_context(const RecordLimits &limits, const TraceContext &context) noexcept;

struct DataChunk {
    DataChunk *next = nullptr;
    std::size_t capacity = 0;
    std::size_t used = 0;

    [[nodiscard]] char *data() noexcept { return reinterpret_cast<char *>(this + 1); }
    [[nodiscard]] const char *data() const noexcept { return reinterpret_cast<const char *>(this + 1); }
};

struct MessageData {
    MessageTrace *trace = nullptr;
    MessageKind kind = MessageKind::Event;
    std::string_view type;
    std::string_view name;
    std::string_view status{fiber::cat::status::Success};
    DataChunk *data_head = nullptr;
    DataChunk *data_tail = nullptr;
    std::chrono::steady_clock::time_point time{};
    std::size_t data_size = 0;
    char data_separator = '&';
    bool has_data = false;
    bool completed = false;
};

struct EventData final : MessageData {
    EventData() noexcept { kind = MessageKind::Event; }
};

struct MetricMessageData final : MessageData {
    MetricMessageData() noexcept { kind = MessageKind::Metric; }
};

struct HeartbeatData final : MessageData {
    HeartbeatData() noexcept { kind = MessageKind::Heartbeat; }
};

struct ChildrenChunk {
    ChildrenChunk *next = nullptr;
    MessageData *children[kChildrenPerChunk];
};

struct TransactionData final : MessageData {
    TransactionData() noexcept { kind = MessageKind::Transaction; }

    ChildrenChunk *children_head = nullptr;
    ChildrenChunk *children_tail = nullptr;
    std::chrono::microseconds duration{};
    std::size_t child_count = 0;
    bool explicit_duration = false;
};

struct MessageHandleAccess {
    [[nodiscard]] static Transaction transaction(TransactionData *data) noexcept;
    [[nodiscard]] static Event event(EventData *data) noexcept;
};

struct ContextEntry {
    std::uint64_t hash = 0;
    std::string_view key;
    char *value_data = nullptr;
    std::size_t value_size = 0;
    std::size_t value_capacity = 0;
    ContextEntry *next_bucket = nullptr;
    ContextEntry *next_all = nullptr;
    ContextEntry *prev_all = nullptr;

    [[nodiscard]] std::string_view value() const noexcept {
        return value_size == 0 ? std::string_view{} : std::string_view(value_data, value_size);
    }
};

struct ContextTable {
    ContextEntry **buckets = nullptr;
    std::size_t bucket_count = 0;
    ContextEntry *all_head = nullptr;
    ContextEntry *all_tail = nullptr;
    std::size_t size = 0;
    std::size_t allocated_bytes = 0;
    std::uint64_t version = 0;
};

struct MessageTraceData {
    explicit MessageTraceData(mem::BufPool &value_pool) noexcept : pool(value_pool) {}

    mem::BufPool &pool;
    std::shared_ptr<CatClientCore> core;
    AggregationShard *aggregation_shard = nullptr;
    event::EventLoop *owner = nullptr;
    RecordLimits limits;
    MessageData *root = nullptr;
    MessageTraceContext propagation_context;
    std::chrono::steady_clock::time_point steady_base{};
    std::uint64_t wall_base_millis = 0;
    std::size_t payload_bytes = 0;
    std::size_t message_count = 0;
    std::size_t open_message_count = 0;
    ContextTable context;
    bool has_problem = false;
    bool truncated = false;
    std::uint64_t dropped_message_count = 0;
    std::uint64_t dropped_data_bytes = 0;
    RecordError first_truncation_reason = RecordError::None;
};

struct MessageTrace {
    MessageTrace() = default;

    MessageTrace(const MessageTrace &) = delete;
    MessageTrace &operator=(const MessageTrace &) = delete;

    MessageTraceData *data = nullptr;
};

[[nodiscard]] std::expected<MessageTrace *, RecordError> create_message_trace(mem::BufPool &pool, RecordLimits limits,
                                                                              TraceContext context = {}) noexcept;
void discard_message_trace(MessageTrace *trace) noexcept;

using ContextVisitorFn = bool (*)(void *, std::string_view, std::string_view) noexcept;

RecordError put_context(MessageTrace &trace, std::string_view key, std::string_view value) noexcept;
[[nodiscard]] std::expected<std::optional<std::string_view>, RecordError> get_context(const MessageTrace &trace,
                                                                                      std::string_view key) noexcept;
[[nodiscard]] std::expected<bool, RecordError> remove_context(MessageTrace &trace, std::string_view key) noexcept;
RecordError for_each_context(MessageTrace &trace, void *opaque, ContextVisitorFn visitor) noexcept;

[[nodiscard]] std::expected<TransactionData *, RecordError>
create_transaction_root(MessageTrace &trace, std::string_view type, std::string_view name) noexcept;
[[nodiscard]] std::expected<EventData *, RecordError> create_event_root(MessageTrace &trace, std::string_view type,
                                                                        std::string_view name) noexcept;

[[nodiscard]] std::expected<TransactionData *, RecordError>
create_transaction_root(mem::BufPool &pool, std::string_view type, std::string_view name, RecordLimits limits,
                        TraceContext context = {}) noexcept;
[[nodiscard]] std::expected<EventData *, RecordError> create_event_root(mem::BufPool &pool, std::string_view type,
                                                                        std::string_view name, RecordLimits limits,
                                                                        TraceContext context = {}) noexcept;

[[nodiscard]] std::expected<TransactionData *, RecordError>
create_transaction(TransactionData &parent, std::string_view type, std::string_view name) noexcept;
[[nodiscard]] std::expected<EventData *, RecordError> create_event(TransactionData &parent, std::string_view type,
                                                                   std::string_view name) noexcept;
[[nodiscard]] std::expected<MetricMessageData *, RecordError>
create_metric(TransactionData &parent, std::string_view type, std::string_view name) noexcept;
[[nodiscard]] std::expected<HeartbeatData *, RecordError>
create_heartbeat(TransactionData &parent, std::string_view type, std::string_view name) noexcept;

[[nodiscard]] std::expected<MetricMessageData *, RecordError>
create_metric_root(mem::BufPool &pool, std::string_view type, std::string_view name, RecordLimits limits,
                   TraceContext context = {}) noexcept;
[[nodiscard]] std::expected<HeartbeatData *, RecordError>
create_heartbeat_root(mem::BufPool &pool, std::string_view type, std::string_view name, RecordLimits limits,
                      TraceContext context = {}) noexcept;

RecordError add_data(MessageData *message, std::string_view data) noexcept;
RecordError add_data(MessageData *message, std::string_view key, std::string_view value) noexcept;
RecordError set_data_separator(MessageData *message, char separator) noexcept;
RecordError set_type(MessageData *message, std::string_view value) noexcept;
RecordError set_name(MessageData *message, std::string_view value) noexcept;
RecordError set_status(MessageData *message, std::string_view value) noexcept;
RecordError set_timestamp(MessageData *message, std::uint64_t timestamp_millis) noexcept;
RecordError set_duration(TransactionData *transaction, std::chrono::microseconds duration) noexcept;
RecordError complete_with_duration(TransactionData *&transaction, std::chrono::microseconds duration,
                                   std::string_view status, std::string_view data) noexcept;

RecordError complete(EventData *&event) noexcept;
RecordError complete(TransactionData *&transaction) noexcept;
void abandon(EventData *&event) noexcept;
void abandon(TransactionData *&transaction) noexcept;

} // namespace fiber::cat::detail

#endif // FIBER_CAT_INTERNAL_H
