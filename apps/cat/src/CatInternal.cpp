#include "CatInternal.h"

#include <algorithm>
#include <chrono>
#include <concepts>
#include <limits>
#include <new>
#include <utility>

#include <common/Assert.h>

#include "CatClientCore.h"

namespace fiber::cat::detail {

namespace {

inline constexpr std::size_t kInitialDataChunkCapacity = 128;

bool checked_add(std::size_t lhs, std::size_t rhs, std::size_t &result) noexcept {
    if (lhs > std::numeric_limits<std::size_t>::max() - rhs) {
        return false;
    }
    result = lhs + rhs;
    return true;
}

bool can_charge(const MessageTraceData &trace, std::size_t bytes) noexcept {
    return trace.payload_bytes <= trace.limits.max_tree_bytes &&
           bytes <= trace.limits.max_tree_bytes - trace.payload_bytes;
}

bool on_owner_loop(const MessageTraceData &trace) noexcept { return trace.owner && trace.owner->in_loop(); }

bool valid_limits(const RecordLimits &limits) noexcept {
    return limits.max_messages > 0 && limits.max_children_per_transaction > 0 && limits.max_type_bytes > 0 &&
           limits.max_name_bytes > 0 && limits.max_status_bytes > 0 && limits.max_data_bytes_per_message > 0 &&
           limits.max_tree_bytes >= sizeof(TransactionData);
}

StringRef literal_ref(std::string_view value) noexcept { return {value.data(), value.size()}; }

StringRef copy_string(MessageTrace &trace, std::string_view value) noexcept {
    if (value.empty()) {
        return literal_ref("");
    }
    auto *copy = static_cast<char *>(trace.pool.alloc(value.size(), alignof(char)));
    if (!copy) {
        return {};
    }
    std::copy(value.begin(), value.end(), copy);
    return {copy, value.size()};
}

std::expected<MessageTrace *, RecordError> create_trace(RecordLimits limits, TraceContext context) noexcept {
    event::EventLoop *loop = event::EventLoop::current_or_null();
    if (!loop) {
        return std::unexpected(RecordError::WrongEventLoop);
    }
    if (!valid_limits(limits)) {
        return std::unexpected(RecordError::InvalidArgument);
    }

    auto *trace = new (std::nothrow) MessageTrace;
    if (!trace) {
        return std::unexpected(RecordError::NoMemory);
    }
    auto *storage = trace->pool.alloc<MessageTraceData>();
    if (!storage) {
        delete trace;
        return std::unexpected(RecordError::NoMemory);
    }
    trace->data = new (storage) MessageTraceData;
    trace->data->core = std::move(context.core);
    trace->data->owner = loop;
    trace->data->limits = limits;
    trace->data->steady_base = loop->now();

    const auto wall_now = std::chrono::system_clock::now().time_since_epoch();
    const auto wall_millis = std::chrono::duration_cast<std::chrono::milliseconds>(wall_now).count();
    trace->data->wall_base_millis = wall_millis > 0 ? static_cast<std::uint64_t>(wall_millis) : 0;

    std::size_t context_bytes = 0;
    if (!checked_add(context.message_id.size(), context.root_message_id.size(), context_bytes) ||
        !checked_add(context_bytes, context.parent_message_id.size(), context_bytes) ||
        !checked_add(context_bytes, context.session_token.size(), context_bytes) ||
        !can_charge(*trace->data, context_bytes)) {
        delete trace;
        return std::unexpected(RecordError::LimitExceeded);
    }
    trace->data->message_id = copy_string(*trace, context.message_id);
    trace->data->root_message_id = copy_string(*trace, context.root_message_id);
    trace->data->parent_message_id = copy_string(*trace, context.parent_message_id);
    trace->data->session_token = copy_string(*trace, context.session_token);
    if ((!context.message_id.empty() && !trace->data->message_id.data) ||
        (!context.root_message_id.empty() && !trace->data->root_message_id.data) ||
        (!context.parent_message_id.empty() && !trace->data->parent_message_id.data) ||
        (!context.session_token.empty() && !trace->data->session_token.data)) {
        delete trace;
        return std::unexpected(RecordError::NoMemory);
    }
    trace->data->payload_bytes = context_bytes;
    return trace;
}

bool message_charge(std::size_t node_size, std::string_view type, std::string_view name, std::size_t &charge) noexcept {
    return checked_add(node_size, type.size(), charge) && checked_add(charge, name.size(), charge);
}

RecordError validate_message(const MessageTraceData &trace, std::string_view type, std::string_view name,
                             std::size_t node_size, std::size_t extra_charge, std::size_t &node_charge) noexcept {
    if (type.size() > trace.limits.max_type_bytes || name.size() > trace.limits.max_name_bytes) {
        return RecordError::LimitExceeded;
    }
    if (trace.message_count >= trace.limits.max_messages) {
        return RecordError::LimitExceeded;
    }
    std::size_t total_charge = 0;
    if (!message_charge(node_size, type, name, node_charge) || !checked_add(node_charge, extra_charge, total_charge) ||
        !can_charge(trace, total_charge)) {
        return RecordError::LimitExceeded;
    }
    return RecordError::None;
}

template<typename Node>
Node *allocate_message(MessageTrace &trace, std::string_view type, std::string_view name,
                       std::size_t node_charge) noexcept {
    void *storage = trace.pool.alloc(node_charge, alignof(Node));
    if (!storage) {
        return nullptr;
    }
    auto *node = new (storage) Node;
    auto *text = static_cast<char *>(storage) + sizeof(Node);
    if (!type.empty()) {
        std::copy(type.begin(), type.end(), text);
        node->type = {text, type.size()};
        text += type.size();
    } else {
        node->type = literal_ref("");
    }
    if (!name.empty()) {
        std::copy(name.begin(), name.end(), text);
        node->name = {text, name.size()};
    } else {
        node->name = literal_ref("");
    }
    node->trace = &trace;
    node->time = event::EventLoop::current().now();
    return node;
}

template<typename Node>
std::expected<Node *, RecordError> create_root(std::string_view type, std::string_view name, RecordLimits limits,
                                               TraceContext context) noexcept {
    auto created_trace = create_trace(limits, std::move(context));
    if (!created_trace) {
        return std::unexpected(created_trace.error());
    }
    MessageTrace *trace = *created_trace;
    std::size_t node_charge = 0;
    const RecordError validation = validate_message(*trace->data, type, name, sizeof(Node), 0, node_charge);
    if (validation != RecordError::None) {
        delete trace;
        return std::unexpected(validation);
    }
    Node *root = allocate_message<Node>(*trace, type, name, node_charge);
    if (!root) {
        delete trace;
        return std::unexpected(RecordError::NoMemory);
    }
    trace->data->root = root;
    trace->data->payload_bytes += node_charge;
    trace->data->message_count = 1;
    trace->data->open_message_count = 1;
    return root;
}

RecordError validate_mutation(const MessageData *message) noexcept {
    if (!message) {
        return RecordError::Completed;
    }
    const MessageTraceData &trace = *message->trace->data;
    if (!on_owner_loop(trace)) {
        return RecordError::WrongEventLoop;
    }
    if (message->completed) {
        return RecordError::Completed;
    }
    return RecordError::None;
}

std::size_t preferred_data_capacity(const MessageData &message, std::size_t needed,
                                    std::size_t remaining_limit) noexcept {
    std::size_t preferred = kInitialDataChunkCapacity;
    if (message.data_tail) {
        if (message.data_tail->capacity <= std::numeric_limits<std::size_t>::max() / 2) {
            preferred = message.data_tail->capacity * 2;
        } else {
            preferred = remaining_limit;
        }
    }
    preferred = std::min(preferred, remaining_limit);
    return std::max(preferred, needed);
}

RecordError append_data(MessageData *message, std::string_view key, std::string_view value, bool key_value) noexcept {
    const RecordError mutable_result = validate_mutation(message);
    if (mutable_result != RecordError::None) {
        return mutable_result;
    }

    MessageTraceData &trace_data = *message->trace->data;
    std::size_t rendered_size = key.size();
    if (key_value &&
        (!checked_add(rendered_size, 1, rendered_size) || !checked_add(rendered_size, value.size(), rendered_size))) {
        return RecordError::LimitExceeded;
    }
    const std::size_t separator_size = message->has_data ? 1 : 0;
    std::size_t needed = 0;
    if (!checked_add(separator_size, rendered_size, needed) ||
        message->data_size > trace_data.limits.max_data_bytes_per_message ||
        needed > trace_data.limits.max_data_bytes_per_message - message->data_size) {
        return RecordError::LimitExceeded;
    }

    DataChunk *chunk = message->data_tail;
    DataChunk *new_chunk = nullptr;
    std::size_t allocation_charge = 0;
    if (needed > 0 && (!chunk || needed > chunk->capacity - chunk->used)) {
        const std::size_t remaining_limit = trace_data.limits.max_data_bytes_per_message - message->data_size;
        std::size_t capacity = preferred_data_capacity(*message, needed, remaining_limit);
        if (!checked_add(sizeof(DataChunk), capacity, allocation_charge) ||
            !can_charge(trace_data, allocation_charge)) {
            capacity = needed;
            if (!checked_add(sizeof(DataChunk), capacity, allocation_charge) ||
                !can_charge(trace_data, allocation_charge)) {
                return RecordError::LimitExceeded;
            }
        }
        void *storage = message->trace->pool.alloc(allocation_charge, alignof(DataChunk));
        if (!storage) {
            return RecordError::NoMemory;
        }
        new_chunk = new (storage) DataChunk;
        new_chunk->capacity = capacity;
        chunk = new_chunk;
    }

    if (needed > 0) {
        char *out = chunk->data() + chunk->used;
        if (separator_size != 0) {
            *out++ = '&';
        }
        std::copy(key.begin(), key.end(), out);
        out += key.size();
        if (key_value) {
            *out++ = '=';
            std::copy(value.begin(), value.end(), out);
        }
        chunk->used += needed;
    }

    if (new_chunk) {
        if (message->data_tail) {
            message->data_tail->next = new_chunk;
        } else {
            message->data_head = new_chunk;
        }
        message->data_tail = new_chunk;
        trace_data.payload_bytes += allocation_charge;
    }
    message->data_size += needed;
    message->has_data = true;
    return RecordError::None;
}

bool timestamp_to_steady(const MessageTraceData &trace, std::uint64_t timestamp_millis,
                         std::chrono::steady_clock::time_point &result) noexcept {
    using Milliseconds = std::chrono::milliseconds;
    using SteadyDuration = std::chrono::steady_clock::duration;

    const auto base_millis = std::chrono::duration_cast<Milliseconds>(trace.steady_base.time_since_epoch()).count();
    std::int64_t delta = 0;
    if (timestamp_millis >= trace.wall_base_millis) {
        const std::uint64_t value = timestamp_millis - trace.wall_base_millis;
        if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return false;
        }
        delta = static_cast<std::int64_t>(value);
    } else {
        const std::uint64_t value = trace.wall_base_millis - timestamp_millis;
        if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return false;
        }
        delta = -static_cast<std::int64_t>(value);
    }

    if ((delta > 0 && base_millis > std::numeric_limits<std::int64_t>::max() - delta) ||
        (delta < 0 && base_millis < std::numeric_limits<std::int64_t>::min() - delta)) {
        return false;
    }
    const std::int64_t target_millis = base_millis + delta;
    const auto min_millis = std::chrono::duration_cast<Milliseconds>(SteadyDuration::min()).count();
    const auto max_millis = std::chrono::duration_cast<Milliseconds>(SteadyDuration::max()).count();
    if (target_millis < min_millis || target_millis > max_millis) {
        return false;
    }
    result = std::chrono::steady_clock::time_point(
            std::chrono::duration_cast<SteadyDuration>(Milliseconds(target_millis)));
    return true;
}

void mark_completed(MessageData &message) noexcept {
    MessageTrace *trace = message.trace;
    MessageTraceData &trace_data = *trace->data;
    FIBER_ASSERT(!message.completed);
    FIBER_ASSERT(trace_data.open_message_count > 0);

    message.completed = true;
    --trace_data.open_message_count;
    if (message.status.view() != status::Success) {
        trace_data.has_problem = true;
    }
    if (trace_data.open_message_count != 0) {
        return;
    }

    std::shared_ptr<CatClientCore> core = std::move(trace_data.core);
    if (core) {
        auto encoded = encode_nt1(trace_data, core->encode_context());
        if (encoded) {
            core->submit_encoded(std::move(*encoded));
        } else {
            core->on_encode_failure(encoded.error());
        }
    }
    delete trace;
}

void finish_transaction(TransactionData &transaction) noexcept {
    if (!transaction.explicit_duration) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(event::EventLoop::current().now() -
                                                                                   transaction.time);
        transaction.duration = std::max(elapsed, std::chrono::microseconds::zero());
    }
    mark_completed(transaction);
}

template<typename Data>
void abandon_message(Data *&handle) noexcept {
    if (!handle) {
        return;
    }
    MessageTraceData &trace = *handle->trace->data;
    FIBER_ASSERT(on_owner_loop(trace));
    Data *message = std::exchange(handle, nullptr);
    message->status = literal_ref(status::Incomplete);
    if constexpr (std::same_as<Data, TransactionData>) {
        finish_transaction(*message);
    } else {
        mark_completed(*message);
    }
}

} // namespace

MessageTrace::~MessageTrace() {
    if (data) {
        std::destroy_at(data);
        data = nullptr;
    }
}

std::expected<TransactionData *, RecordError> create_transaction_root(std::string_view type, std::string_view name,
                                                                      RecordLimits limits,
                                                                      TraceContext context) noexcept {
    return create_root<TransactionData>(type, name, limits, std::move(context));
}

std::expected<EventData *, RecordError> create_event_root(std::string_view type, std::string_view name,
                                                          RecordLimits limits, TraceContext context) noexcept {
    return create_root<EventData>(type, name, limits, std::move(context));
}

template<typename Node>
std::expected<Node *, RecordError> create_child(TransactionData &parent, std::string_view type,
                                                std::string_view name) noexcept {
    MessageTrace &trace = *parent.trace;
    MessageTraceData &trace_data = *trace.data;
    if (!on_owner_loop(trace_data)) {
        return std::unexpected(RecordError::WrongEventLoop);
    }
    if (parent.completed) {
        return std::unexpected(RecordError::Completed);
    }
    if (parent.child_count >= trace_data.limits.max_children_per_transaction) {
        return std::unexpected(RecordError::LimitExceeded);
    }

    const bool needs_chunk = parent.child_count % kChildrenPerChunk == 0;
    const std::size_t chunk_charge = needs_chunk ? sizeof(ChildrenChunk) : 0;
    std::size_t node_charge = 0;
    const RecordError validation = validate_message(trace_data, type, name, sizeof(Node), chunk_charge, node_charge);
    if (validation != RecordError::None) {
        return std::unexpected(validation);
    }

    Node *child = allocate_message<Node>(trace, type, name, node_charge);
    if (!child) {
        return std::unexpected(RecordError::NoMemory);
    }

    ChildrenChunk *new_chunk = nullptr;
    if (needs_chunk) {
        auto *storage = trace.pool.alloc<ChildrenChunk>();
        if (!storage) {
            trace_data.payload_bytes += node_charge;
            return std::unexpected(RecordError::NoMemory);
        }
        new_chunk = new (storage) ChildrenChunk;
    }

    if (new_chunk) {
        if (parent.children_tail) {
            parent.children_tail->next = new_chunk;
        } else {
            parent.children_head = new_chunk;
        }
        parent.children_tail = new_chunk;
    }
    const std::size_t child_index = parent.child_count % kChildrenPerChunk;
    parent.children_tail->children[child_index] = child;
    ++parent.child_count;

    trace_data.payload_bytes += node_charge + chunk_charge;
    ++trace_data.message_count;
    ++trace_data.open_message_count;
    return child;
}

std::expected<TransactionData *, RecordError> create_transaction(TransactionData &parent, std::string_view type,
                                                                 std::string_view name) noexcept {
    return create_child<TransactionData>(parent, type, name);
}

std::expected<EventData *, RecordError> create_event(TransactionData &parent, std::string_view type,
                                                     std::string_view name) noexcept {
    return create_child<EventData>(parent, type, name);
}

RecordError add_data(MessageData *message, std::string_view data) noexcept {
    return append_data(message, data, {}, false);
}

RecordError add_data(MessageData *message, std::string_view key, std::string_view value) noexcept {
    return append_data(message, key, value, true);
}

RecordError set_status(MessageData *message, std::string_view value) noexcept {
    const RecordError mutable_result = validate_mutation(message);
    if (mutable_result != RecordError::None) {
        return mutable_result;
    }
    MessageTraceData &trace_data = *message->trace->data;
    if (value.size() > trace_data.limits.max_status_bytes || !can_charge(trace_data, value.size())) {
        return RecordError::LimitExceeded;
    }

    if (value == status::Success) {
        message->status = literal_ref(status::Success);
        return RecordError::None;
    }
    if (value == status::Incomplete) {
        message->status = literal_ref(status::Incomplete);
        return RecordError::None;
    }
    StringRef copy = copy_string(*message->trace, value);
    if (!value.empty() && !copy.data) {
        return RecordError::NoMemory;
    }
    message->status = copy;
    trace_data.payload_bytes += value.size();
    return RecordError::None;
}

RecordError set_timestamp(MessageData *message, std::uint64_t timestamp_millis) noexcept {
    const RecordError mutable_result = validate_mutation(message);
    if (mutable_result != RecordError::None) {
        return mutable_result;
    }
    std::chrono::steady_clock::time_point converted;
    if (!timestamp_to_steady(*message->trace->data, timestamp_millis, converted)) {
        return RecordError::LimitExceeded;
    }
    message->time = converted;
    return RecordError::None;
}

RecordError set_duration(TransactionData *transaction, std::chrono::microseconds duration) noexcept {
    const RecordError mutable_result = validate_mutation(transaction);
    if (mutable_result != RecordError::None) {
        return mutable_result;
    }
    if (duration.count() < 0) {
        return RecordError::InvalidArgument;
    }
    transaction->duration = duration;
    transaction->explicit_duration = true;
    return RecordError::None;
}

RecordError complete(EventData *&event) noexcept {
    if (!event) {
        return RecordError::None;
    }
    if (!on_owner_loop(*event->trace->data)) {
        return RecordError::WrongEventLoop;
    }
    EventData *message = std::exchange(event, nullptr);
    mark_completed(*message);
    return RecordError::None;
}

RecordError complete(TransactionData *&transaction) noexcept {
    if (!transaction) {
        return RecordError::None;
    }
    if (!on_owner_loop(*transaction->trace->data)) {
        return RecordError::WrongEventLoop;
    }
    TransactionData *message = std::exchange(transaction, nullptr);
    finish_transaction(*message);
    return RecordError::None;
}

void abandon(EventData *&event) noexcept { abandon_message(event); }

void abandon(TransactionData *&transaction) noexcept { abandon_message(transaction); }

} // namespace fiber::cat::detail
