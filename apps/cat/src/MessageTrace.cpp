#include <fiber/cat/MessageTrace.h>

#include <algorithm>
#include <limits>
#include <utility>

#include <fiber/cat/CatClient.h>

#include "CatClientCore.h"
#include "CatInternal.h"

namespace fiber::cat {

namespace {

bool checked_add(std::size_t lhs, std::size_t rhs, std::size_t &result) noexcept {
    if (lhs > std::numeric_limits<std::size_t>::max() - rhs) {
        return false;
    }
    result = lhs + rhs;
    return true;
}

std::expected<MessageTraceContext, RecordError> copy_context(mem::BufPool &pool, MessageTraceContext context) noexcept {
    std::size_t size = 0;
    if (!checked_add(context.message_id.size(), context.root_message_id.size(), size) ||
        !checked_add(size, context.parent_message_id.size(), size) ||
        !checked_add(size, context.session_token.size(), size)) {
        return std::unexpected(RecordError::LimitExceeded);
    }
    if (size == 0) {
        return MessageTraceContext{};
    }
    char *storage = pool.alloc<char>(size);
    if (!storage) {
        return std::unexpected(RecordError::NoMemory);
    }
    char *output = storage;
    const auto copy_field = [&output](std::string_view value) noexcept {
        if (value.empty()) {
            return std::string_view{};
        }
        std::copy(value.begin(), value.end(), output);
        const std::string_view copied(output, value.size());
        output += value.size();
        return copied;
    };
    return MessageTraceContext{
            .message_id = copy_field(context.message_id),
            .root_message_id = copy_field(context.root_message_id),
            .parent_message_id = copy_field(context.parent_message_id),
            .session_token = copy_field(context.session_token),
    };
}

} // namespace

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
            .propagation_context = options.context,
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

std::expected<MessageTraceContext, RecordError> MessageTrace::propagation_context() const noexcept {
    if (!trace_ || !trace_->data) {
        return std::unexpected(RecordError::Completed);
    }
    const detail::MessageTraceData &data = *trace_->data;
    if (!data.owner || !data.owner->in_loop()) {
        return std::unexpected(RecordError::WrongEventLoop);
    }
    return data.propagation_context;
}

std::expected<MessageTraceContext, RecordError>
MessageTrace::copy_propagation_context(mem::BufPool &destination_pool) const noexcept {
    auto context = propagation_context();
    if (!context) {
        return std::unexpected(context.error());
    }
    const detail::MessageTraceData &data = *trace_->data;
    if (&destination_pool == &data.pool) {
        return *context;
    }
    auto copied = copy_context(destination_pool, *context);
    if (!copied) {
        data.core->on_context_failure(copied.error());
    }
    return copied;
}

std::expected<MessageTraceContext, RecordError>
MessageTrace::create_remote_context(mem::BufPool &destination_pool, std::string_view remote_domain) const noexcept {
    auto context = propagation_context();
    if (!context) {
        return std::unexpected(context.error());
    }
    detail::MessageTraceData &data = *trace_->data;
    if (context->message_id.empty()) {
        data.core->on_context_failure(RecordError::InvalidContext);
        return std::unexpected(RecordError::InvalidContext);
    }
    auto child_id = data.core->create_message_id(remote_domain);
    if (!child_id) {
        return std::unexpected(child_id.error());
    }
    const MessageTraceContext remote{
            .message_id = child_id->view(),
            .root_message_id = context->root_message_id.empty() ? context->message_id : context->root_message_id,
            .parent_message_id = context->message_id,
            .session_token = context->session_token,
    };
    if (&destination_pool != &data.pool) {
        auto copied = copy_context(destination_pool, remote);
        if (!copied) {
            data.core->on_context_failure(copied.error());
        }
        return copied;
    }

    auto copied_id = copy_context(destination_pool, {.message_id = remote.message_id});
    if (!copied_id) {
        data.core->on_context_failure(copied_id.error());
        return std::unexpected(copied_id.error());
    }
    return MessageTraceContext{
            .message_id = copied_id->message_id,
            .root_message_id = remote.root_message_id,
            .parent_message_id = remote.parent_message_id,
            .session_token = remote.session_token,
    };
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
