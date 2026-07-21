#ifndef FIBER_CAT_INTERNAL_H
#define FIBER_CAT_INTERNAL_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string_view>

#include <common/mem/BufPool.h>
#include <event/EventLoop.h>
#include <fiber/cat/Message.h>
#include <fiber/cat/Status.h>

namespace fiber::cat::detail {

inline constexpr std::size_t kChildrenPerChunk = 16;

class CatClientCore;
struct MessageTrace;

struct TraceContext {
    std::shared_ptr<CatClientCore> core;
    std::string_view message_id;
    std::string_view root_message_id;
    std::string_view parent_message_id;
    std::string_view session_token;
};

struct StringRef {
    const char *data = nullptr;
    std::size_t size = 0;

    [[nodiscard]] std::string_view view() const noexcept { return {data, size}; }
};

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
    StringRef type;
    StringRef name;
    StringRef status{status::Success.data(), status::Success.size()};
    DataChunk *data_head = nullptr;
    DataChunk *data_tail = nullptr;
    std::chrono::steady_clock::time_point time{};
    std::size_t data_size = 0;
    bool has_data = false;
    bool completed = false;
};

struct EventData final : MessageData {
    EventData() noexcept { kind = MessageKind::Event; }
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

struct MessageTraceData {
    std::shared_ptr<CatClientCore> core;
    event::EventLoop *owner = nullptr;
    RecordLimits limits;
    MessageData *root = nullptr;
    StringRef message_id;
    StringRef root_message_id;
    StringRef parent_message_id;
    StringRef session_token;
    std::chrono::steady_clock::time_point steady_base{};
    std::uint64_t wall_base_millis = 0;
    std::size_t payload_bytes = 0;
    std::size_t message_count = 0;
    std::size_t open_message_count = 0;
    bool has_problem = false;
};

struct MessageTrace {
    MessageTrace() = default;
    ~MessageTrace();

    MessageTrace(const MessageTrace &) = delete;
    MessageTrace &operator=(const MessageTrace &) = delete;

    mem::BufPool pool;
    MessageTraceData *data = nullptr;
};

[[nodiscard]] std::expected<TransactionData *, RecordError> create_transaction_root(std::string_view type,
                                                                                    std::string_view name,
                                                                                    RecordLimits limits,
                                                                                    TraceContext context = {}) noexcept;
[[nodiscard]] std::expected<EventData *, RecordError> create_event_root(std::string_view type, std::string_view name,
                                                                        RecordLimits limits,
                                                                        TraceContext context = {}) noexcept;

[[nodiscard]] std::expected<TransactionData *, RecordError>
create_transaction(TransactionData &parent, std::string_view type, std::string_view name) noexcept;
[[nodiscard]] std::expected<EventData *, RecordError> create_event(TransactionData &parent, std::string_view type,
                                                                   std::string_view name) noexcept;

RecordError add_data(MessageData *message, std::string_view data) noexcept;
RecordError add_data(MessageData *message, std::string_view key, std::string_view value) noexcept;
RecordError set_status(MessageData *message, std::string_view value) noexcept;
RecordError set_timestamp(MessageData *message, std::uint64_t timestamp_millis) noexcept;
RecordError set_duration(TransactionData *transaction, std::chrono::microseconds duration) noexcept;

RecordError complete(EventData *&event) noexcept;
RecordError complete(TransactionData *&transaction) noexcept;
void abandon(EventData *&event) noexcept;
void abandon(TransactionData *&transaction) noexcept;

} // namespace fiber::cat::detail

#endif // FIBER_CAT_INTERNAL_H
