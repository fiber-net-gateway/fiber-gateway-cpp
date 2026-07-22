#ifndef FIBER_CAT_MESSAGE_TRACE_H
#define FIBER_CAT_MESSAGE_TRACE_H

#include <expected>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>

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

    RecordError put_context(std::string_view key, std::string_view value) noexcept;
    [[nodiscard]] std::expected<std::optional<std::string_view>, RecordError>
    get_context(std::string_view key) const noexcept;
    [[nodiscard]] std::expected<bool, RecordError> remove_context(std::string_view key) noexcept;

    template<typename Visitor>
    RecordError for_each_context(Visitor &&visitor) const noexcept {
        using VisitorType = std::remove_reference_t<Visitor>;
        static_assert(std::is_nothrow_invocable_r_v<bool, VisitorType &, std::string_view, std::string_view>,
                      "MessageTrace context visitor must be noexcept and return bool");

        void *opaque = const_cast<void *>(static_cast<const void *>(std::addressof(visitor)));
        return for_each_context_impl(opaque,
                                     [](void *value, std::string_view key, std::string_view context_value) noexcept {
                                         return (*static_cast<VisitorType *>(value))(key, context_value);
                                     });
    }

    [[nodiscard]] std::expected<Transaction, RecordError> create_transaction(std::string_view type,
                                                                             std::string_view name) noexcept;
    [[nodiscard]] std::expected<Event, RecordError> create_event(std::string_view type, std::string_view name) noexcept;

private:
    using ContextVisitorFn = bool (*)(void *, std::string_view, std::string_view) noexcept;

    explicit MessageTrace(detail::MessageTrace *trace) noexcept : trace_(trace) {}

    void reset() noexcept;
    RecordError for_each_context_impl(void *opaque, ContextVisitorFn visitor) const noexcept;

    detail::MessageTrace *trace_ = nullptr;
};

} // namespace fiber::cat

#endif // FIBER_CAT_MESSAGE_TRACE_H
