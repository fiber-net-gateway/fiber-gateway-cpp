#ifndef FIBER_CAT_MESSAGE_TRACE_H
#define FIBER_CAT_MESSAGE_TRACE_H

#include <expected>
#include <string_view>

#include "Event.h"
#include "Message.h"
#include "Transaction.h"

namespace fiber::cat {

class CatClient;

namespace detail {
struct MessageTrace;
}

struct MessageTraceContext {
    std::string_view message_id;
    std::string_view root_message_id;
    std::string_view parent_message_id;
    std::string_view session_token;
};

class MessageTrace {
public:
    MessageTrace() noexcept = default;
    MessageTrace(const MessageTrace &) = delete;
    MessageTrace &operator=(const MessageTrace &) = delete;
    MessageTrace(MessageTrace &&other) noexcept;
    MessageTrace &operator=(MessageTrace &&other) noexcept;
    ~MessageTrace();

    [[nodiscard]] static std::expected<MessageTrace, RecordError> create(CatClient &client, RecordLimits limits = {},
                                                                         MessageTraceContext context = {}) noexcept;

    [[nodiscard]] bool valid() const noexcept;

    [[nodiscard]] std::expected<Transaction, RecordError> create_transaction(std::string_view type,
                                                                             std::string_view name) noexcept;
    [[nodiscard]] std::expected<Event, RecordError> create_event(std::string_view type, std::string_view name) noexcept;

private:
    explicit MessageTrace(detail::MessageTrace *trace) noexcept : trace_(trace) {}

    void reset() noexcept;

    detail::MessageTrace *trace_ = nullptr;
};

} // namespace fiber::cat

#endif // FIBER_CAT_MESSAGE_TRACE_H
