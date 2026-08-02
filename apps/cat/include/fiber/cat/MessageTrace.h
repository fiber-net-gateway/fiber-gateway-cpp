#ifndef FIBER_CAT_MESSAGE_TRACE_H
#define FIBER_CAT_MESSAGE_TRACE_H

#include <expected>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>

#include "Message.h"

namespace fiber::mem {
class BufPool;
}

namespace fiber::cat {

class CatClient;
class Event;
class Transaction;

struct MessageTraceContext {
    std::string_view message_id;
    std::string_view root_message_id;
    std::string_view parent_message_id;
    std::string_view session_token;
};

struct MessageTraceCreateOptions {
    RecordLimits limits{};
    MessageTraceContext context{};
};

namespace detail {
struct MessageTrace;
}

class MessageTrace {
public:
    MessageTrace() noexcept = default;
    MessageTrace(const MessageTrace &) = delete;
    MessageTrace &operator=(const MessageTrace &) = delete;
    MessageTrace(MessageTrace &&other) noexcept;
    MessageTrace &operator=(MessageTrace &&other) noexcept;
    ~MessageTrace() = default;

    [[nodiscard]] bool valid() const noexcept;
    // The returned views borrow the MessageTrace pool.
    [[nodiscard]] std::expected<MessageTraceContext, RecordError> propagation_context() const noexcept;
    // The returned views borrow destination_pool. Same-pool copies reuse the existing storage.
    [[nodiscard]] std::expected<MessageTraceContext, RecordError>
    copy_propagation_context(mem::BufPool &destination_pool) const noexcept;
    // Creates the child message id in destination_pool. Existing fields are reused when it is the trace pool.
    [[nodiscard]] std::expected<MessageTraceContext, RecordError>
    create_remote_context(mem::BufPool &destination_pool, std::string_view remote_domain = {}) const noexcept;

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

private:
    friend class Event;
    friend class Transaction;

    using ContextVisitorFn = bool (*)(void *, std::string_view, std::string_view) noexcept;

    [[nodiscard]] static std::expected<MessageTrace, RecordError> create(CatClient &client, mem::BufPool &pool,
                                                                         MessageTraceCreateOptions options) noexcept;

    explicit MessageTrace(detail::MessageTrace *trace) noexcept : trace_(trace) {}

    RecordError for_each_context_impl(void *opaque, ContextVisitorFn visitor) const noexcept;

    detail::MessageTrace *trace_ = nullptr;
};

} // namespace fiber::cat

#endif // FIBER_CAT_MESSAGE_TRACE_H
