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
    trace_ = std::exchange(other.trace_, nullptr);
    return *this;
}

std::expected<MessageTrace, RecordError> MessageTrace::create(CatClient &client, mem::BufPool &pool,
                                                              MessageTraceCreateOptions options) noexcept {
    std::shared_ptr<detail::CatClientCore> core = client.core();
    if (!core || !core->accepts_messages()) {
        return std::unexpected(RecordError::Completed);
    }
    event::EventLoop *loop = event::EventLoop::current_or_null();
    if (!loop) {
        return std::unexpected(RecordError::WrongEventLoop);
    }

    std::optional<detail::GeneratedMessageId> generated_id;
    if (options.context.message_id.empty()) {
        auto generated = core->create_message_id();
        if (!generated) {
            return std::unexpected(generated.error());
        }
        generated_id.emplace(std::move(*generated));
        options.context.message_id = generated_id->view();
    }

    detail::TraceContext internal_context{
            .core = core,
            .aggregation_shard = core->aggregation_shard(*loop),
            .message_id = options.context.message_id,
            .root_message_id = options.context.root_message_id,
            .parent_message_id = options.context.parent_message_id,
            .session_token = options.context.session_token,
    };
    const RecordError validation = detail::validate_trace_context(options.limits, internal_context);
    if (validation != RecordError::None) {
        core->on_context_failure(validation);
        return std::unexpected(validation);
    }
    auto created = detail::create_message_trace(pool, options.limits, std::move(internal_context));
    if (!created) {
        if (created.error() == RecordError::NoMemory || created.error() == RecordError::LimitExceeded ||
            created.error() == RecordError::InvalidContext) {
            core->on_context_failure(created.error());
        }
        return std::unexpected(created.error());
    }
    return MessageTrace(*created);
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

} // namespace fiber::cat
