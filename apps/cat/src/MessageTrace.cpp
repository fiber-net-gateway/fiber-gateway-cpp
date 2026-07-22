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
    event::EventLoop *loop = event::EventLoop::current_or_null();
    if (!loop) {
        return std::unexpected(RecordError::WrongEventLoop);
    }

    std::optional<detail::GeneratedMessageId> generated_id;
    if (context.message_id.empty()) {
        if (!context.root_message_id.empty() || !context.parent_message_id.empty()) {
            core->on_context_failure(RecordError::InvalidContext);
            return std::unexpected(RecordError::InvalidContext);
        }
        auto generated = core->create_message_id();
        if (!generated) {
            return std::unexpected(generated.error());
        }
        generated_id.emplace(std::move(*generated));
        context.message_id = generated_id->view();
    }

    detail::TraceContext internal_context{
            .core = core,
            .aggregation_shard = core->aggregation_shard(*loop),
            .message_id = context.message_id,
            .root_message_id = context.root_message_id,
            .parent_message_id = context.parent_message_id,
            .session_token = context.session_token,
    };
    const RecordError validation = detail::validate_trace_context(limits, internal_context);
    if (validation != RecordError::None) {
        core->on_context_failure(validation);
        return std::unexpected(validation);
    }
    auto created = detail::create_message_trace(limits, std::move(internal_context));
    if (!created) {
        if (created.error() == RecordError::NoMemory || created.error() == RecordError::LimitExceeded ||
            created.error() == RecordError::InvalidContext) {
            core->on_context_failure(created.error());
        }
        return std::unexpected(created.error());
    }
    return MessageTrace(*created);
}

std::expected<MessageTrace, RecordError> MessageTrace::create(CatClient &client, const PropagationContext &context,
                                                              RecordLimits limits) noexcept {
    if (!context.valid()) {
        return std::unexpected(RecordError::InvalidContext);
    }
    return create(client, limits, context.view());
}

bool MessageTrace::valid() const noexcept { return trace_ && trace_->data; }

std::expected<PropagationContext, RecordError> MessageTrace::propagation_context() const noexcept {
    if (!trace_ || !trace_->data) {
        return std::unexpected(RecordError::Completed);
    }
    const detail::MessageTraceData &data = *trace_->data;
    if (!data.owner || !data.owner->in_loop()) {
        return std::unexpected(RecordError::WrongEventLoop);
    }
    return PropagationContext::create({
            .message_id = data.message_id.view(),
            .root_message_id = data.root_message_id.view(),
            .parent_message_id = data.parent_message_id.view(),
            .session_token = data.session_token.view(),
    });
}

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

RecordError MessageTrace::log_error(std::string_view message, std::string_view error) noexcept {
    auto created = create_event("Exception", message);
    if (!created) {
        return created.error();
    }
    Event event = std::move(*created);
    RecordError result = event.set_status(status::Error);
    if (!error.empty()) {
        const RecordError data_result = event.add_data(error);
        if (result == RecordError::None) {
            result = data_result;
        }
    }
    const RecordError complete_result = event.complete();
    return result != RecordError::None ? result : complete_result;
}

RecordError MessageTrace::log_completed_transaction(std::string_view type, std::string_view name,
                                                    std::chrono::microseconds duration, std::string_view status_value,
                                                    std::string_view data) noexcept {
    auto created = create_transaction(type, name);
    if (!created) {
        return created.error();
    }
    Transaction transaction = std::move(*created);
    return detail::complete_with_duration(transaction.data_, duration, status_value, data);
}

void MessageTrace::reset() noexcept { detail::release_message_trace(trace_); }

} // namespace fiber::cat
