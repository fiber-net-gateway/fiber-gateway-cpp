#include <fiber/cat/MessageTrace.h>

#include <utility>

#include <fiber/cat/CatClient.h>

#include "CatClientCore.h"
#include "CatInternal.h"

namespace fiber::cat {

MessageTrace::MessageTrace(MessageTrace &&other) noexcept : trace_(std::exchange(other.trace_, nullptr)) {}

MessageTrace &MessageTrace::operator=(MessageTrace &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    reset();
    trace_ = std::exchange(other.trace_, nullptr);
    return *this;
}

MessageTrace::~MessageTrace() { reset(); }

std::expected<MessageTrace, RecordError> MessageTrace::create(CatClient &client, RecordLimits limits,
                                                              MessageTraceContext context) noexcept {
    std::shared_ptr<detail::CatClientCore> core = client.core();
    if (!core || !core->accepts_messages()) {
        return std::unexpected(RecordError::Completed);
    }

    detail::TraceContext internal_context{
            .core = std::move(core),
            .message_id = context.message_id,
            .root_message_id = context.root_message_id,
            .parent_message_id = context.parent_message_id,
            .session_token = context.session_token,
    };
    auto created = detail::create_message_trace(limits, std::move(internal_context));
    if (!created) {
        return std::unexpected(created.error());
    }
    return MessageTrace(*created);
}

bool MessageTrace::valid() const noexcept { return trace_ && trace_->data; }

RecordError MessageTrace::put_context(std::string_view key, std::string_view value) noexcept {
    if (!trace_) {
        return RecordError::Completed;
    }
    return detail::put_context(*trace_, key, value);
}

std::expected<std::optional<std::string_view>, RecordError>
MessageTrace::get_context(std::string_view key) const noexcept {
    if (!trace_) {
        return std::unexpected(RecordError::Completed);
    }
    return detail::get_context(*trace_, key);
}

std::expected<bool, RecordError> MessageTrace::remove_context(std::string_view key) noexcept {
    if (!trace_) {
        return std::unexpected(RecordError::Completed);
    }
    return detail::remove_context(*trace_, key);
}

RecordError MessageTrace::for_each_context_impl(void *opaque, ContextVisitorFn visitor) const noexcept {
    if (!trace_) {
        return RecordError::Completed;
    }
    return detail::for_each_context(*trace_, opaque, visitor);
}

std::expected<Transaction, RecordError> MessageTrace::create_transaction(std::string_view type,
                                                                         std::string_view name) noexcept {
    if (!trace_) {
        return std::unexpected(RecordError::Completed);
    }
    auto created = detail::create_transaction_root(*trace_, type, name);
    if (!created) {
        return std::unexpected(created.error());
    }
    return Transaction(*created);
}

std::expected<Event, RecordError> MessageTrace::create_event(std::string_view type, std::string_view name) noexcept {
    if (!trace_) {
        return std::unexpected(RecordError::Completed);
    }
    auto created = detail::create_event_root(*trace_, type, name);
    if (!created) {
        return std::unexpected(created.error());
    }
    return Event(*created);
}

void MessageTrace::reset() noexcept { detail::release_message_trace(trace_); }

} // namespace fiber::cat
