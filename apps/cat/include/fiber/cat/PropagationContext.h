#ifndef FIBER_CAT_PROPAGATION_CONTEXT_H
#define FIBER_CAT_PROPAGATION_CONTEXT_H

#include <expected>
#include <string_view>

#include "Message.h"

namespace fiber::cat {

namespace detail {
struct PropagationContextData;
}

struct MessageTraceContext {
    std::string_view message_id;
    std::string_view root_message_id;
    std::string_view parent_message_id;
    std::string_view session_token;
};

class PropagationContext {
public:
    PropagationContext() noexcept = default;
    PropagationContext(const PropagationContext &other) noexcept;
    PropagationContext &operator=(const PropagationContext &other) noexcept;
    PropagationContext(PropagationContext &&other) noexcept;
    PropagationContext &operator=(PropagationContext &&other) noexcept;
    ~PropagationContext();

    [[nodiscard]] static std::expected<PropagationContext, RecordError> create(MessageTraceContext context) noexcept;

    [[nodiscard]] bool valid() const noexcept { return data_ != nullptr; }
    [[nodiscard]] std::string_view message_id() const noexcept;
    [[nodiscard]] std::string_view root_message_id() const noexcept;
    [[nodiscard]] std::string_view parent_message_id() const noexcept;
    [[nodiscard]] std::string_view session_token() const noexcept;
    [[nodiscard]] MessageTraceContext view() const noexcept;

private:
    explicit PropagationContext(detail::PropagationContextData *data) noexcept : data_(data) {}

    void reset() noexcept;

    detail::PropagationContextData *data_ = nullptr;
};

} // namespace fiber::cat

#endif // FIBER_CAT_PROPAGATION_CONTEXT_H
