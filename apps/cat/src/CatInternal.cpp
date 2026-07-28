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
inline constexpr std::size_t kMinContextBucketCount = 8;
inline constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
inline constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

bool checked_add(std::size_t lhs, std::size_t rhs, std::size_t &result) noexcept {
    if (lhs > std::numeric_limits<std::size_t>::max() - rhs) {
        return false;
    }
    result = lhs + rhs;
    return true;
}

bool next_power_of_two(std::size_t value, std::size_t &result) noexcept {
    std::size_t power = 1;
    while (power < value) {
        if (power > std::numeric_limits<std::size_t>::max() / 2) {
            return false;
        }
        power *= 2;
    }
    result = power;
    return true;
}

std::uint64_t hash_context_key(std::string_view key) noexcept {
    std::uint64_t hash = kFnvOffsetBasis;
    for (const unsigned char byte: key) {
        hash ^= byte;
        hash *= kFnvPrime;
    }
    return hash;
}

bool can_charge(const MessageTraceData &trace, std::size_t bytes) noexcept {
    return trace.payload_bytes <= trace.limits.max_tree_bytes &&
           bytes <= trace.limits.max_tree_bytes - trace.payload_bytes;
}

bool can_charge_context(const MessageTraceData &trace, std::size_t bytes) noexcept {
    return trace.context.allocated_bytes <= trace.limits.max_context_bytes &&
           bytes <= trace.limits.max_context_bytes - trace.context.allocated_bytes && can_charge(trace, bytes);
}

void mark_truncated(MessageTraceData &trace, std::uint64_t messages, std::uint64_t data_bytes,
                    RecordError reason) noexcept {
    trace.truncated = true;
    trace.has_problem = true;
    if (trace.first_truncation_reason == RecordError::None) {
        trace.first_truncation_reason = reason;
    }
    trace.dropped_message_count = messages > std::numeric_limits<std::uint64_t>::max() - trace.dropped_message_count
                                          ? std::numeric_limits<std::uint64_t>::max()
                                          : trace.dropped_message_count + messages;
    trace.dropped_data_bytes = data_bytes > std::numeric_limits<std::uint64_t>::max() - trace.dropped_data_bytes
                                       ? std::numeric_limits<std::uint64_t>::max()
                                       : trace.dropped_data_bytes + data_bytes;
}

void charge_context(MessageTraceData &trace, std::size_t bytes) noexcept {
    trace.context.allocated_bytes += bytes;
    trace.payload_bytes += bytes;
}

bool on_owner_loop(const MessageTraceData &trace) noexcept { return trace.owner && trace.owner->in_loop(); }

RecordError validate_context_access(const MessageTrace &trace) noexcept {
    if (!trace.data) {
        return RecordError::Completed;
    }
    if (!on_owner_loop(*trace.data)) {
        return RecordError::WrongEventLoop;
    }
    return RecordError::None;
}

ContextEntry *find_context_entry(ContextTable &table, std::string_view key, std::uint64_t hash,
                                 ContextEntry **previous = nullptr) noexcept {
    if (!table.buckets) {
        return nullptr;
    }
    ContextEntry *prev = nullptr;
    ContextEntry *entry = table.buckets[hash & (table.bucket_count - 1)];
    while (entry) {
        if (entry->hash == hash && entry->key.view() == key) {
            if (previous) {
                *previous = prev;
            }
            return entry;
        }
        prev = entry;
        entry = entry->next_bucket;
    }
    if (previous) {
        *previous = nullptr;
    }
    return nullptr;
}

const ContextEntry *find_context_entry(const ContextTable &table, std::string_view key, std::uint64_t hash) noexcept {
    if (!table.buckets) {
        return nullptr;
    }
    const ContextEntry *entry = table.buckets[hash & (table.bucket_count - 1)];
    while (entry) {
        if (entry->hash == hash && entry->key.view() == key) {
            return entry;
        }
        entry = entry->next_bucket;
    }
    return nullptr;
}

RecordError ensure_context_buckets(MessageTrace &trace) noexcept {
    MessageTraceData &data = *trace.data;
    ContextTable &table = data.context;
    if (table.buckets) {
        return RecordError::None;
    }
    if (data.limits.max_context_entries == 0) {
        return RecordError::LimitExceeded;
    }

    std::size_t bucket_count = 0;
    if (!next_power_of_two(std::max(kMinContextBucketCount, data.limits.max_context_entries), bucket_count) ||
        bucket_count > std::numeric_limits<std::size_t>::max() / sizeof(ContextEntry *)) {
        return RecordError::LimitExceeded;
    }
    const std::size_t bucket_bytes = bucket_count * sizeof(ContextEntry *);
    if (!can_charge_context(data, bucket_bytes)) {
        return RecordError::LimitExceeded;
    }

    auto **buckets = static_cast<ContextEntry **>(trace.pool.alloc(bucket_bytes, alignof(ContextEntry *)));
    if (!buckets) {
        return RecordError::NoMemory;
    }
    std::fill_n(buckets, bucket_count, nullptr);
    table.buckets = buckets;
    table.bucket_count = bucket_count;
    charge_context(data, bucket_bytes);
    return RecordError::None;
}

bool valid_limits(const RecordLimits &limits) noexcept {
    return limits.max_messages > 0 && limits.max_children_per_transaction > 0 && limits.max_type_bytes > 0 &&
           limits.max_name_bytes > 0 && limits.max_status_bytes > 0 && limits.max_data_bytes_per_message > 0 &&
           limits.max_message_id_bytes > 0 && limits.max_session_token_bytes > 0 &&
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
    const RecordError context_validation = validate_trace_context(limits, context);
    if (context_validation != RecordError::None) {
        return std::unexpected(context_validation);
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
    trace->data->aggregation_shard = context.aggregation_shard;
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
std::expected<Node *, RecordError> create_root(MessageTrace &trace, std::string_view type,
                                               std::string_view name) noexcept {
    if (!trace.data) {
        return std::unexpected(RecordError::Completed);
    }
    if (!on_owner_loop(*trace.data)) {
        return std::unexpected(RecordError::WrongEventLoop);
    }
    if (trace.data->root) {
        return std::unexpected(RecordError::InvalidArgument);
    }
    std::size_t node_charge = 0;
    const RecordError validation = validate_message(*trace.data, type, name, sizeof(Node), 0, node_charge);
    if (validation != RecordError::None) {
        mark_truncated(*trace.data, 1, type.size() + name.size(), validation);
        return std::unexpected(validation);
    }
    Node *root = allocate_message<Node>(trace, type, name, node_charge);
    if (!root) {
        mark_truncated(*trace.data, 1, type.size() + name.size(), RecordError::NoMemory);
        return std::unexpected(RecordError::NoMemory);
    }
    trace.data->root = root;
    trace.data->payload_bytes += node_charge;
    trace.data->message_count = 1;
    trace.data->open_message_count = 1;
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
        mark_truncated(trace_data, 0, std::numeric_limits<std::uint64_t>::max(), RecordError::LimitExceeded);
        return RecordError::LimitExceeded;
    }
    const std::size_t separator_size = message->has_data ? 1 : 0;
    std::size_t needed = 0;
    if (!checked_add(separator_size, rendered_size, needed) ||
        message->data_size > trace_data.limits.max_data_bytes_per_message ||
        needed > trace_data.limits.max_data_bytes_per_message - message->data_size) {
        mark_truncated(trace_data, 0, rendered_size, RecordError::LimitExceeded);
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
                mark_truncated(trace_data, 0, rendered_size, RecordError::LimitExceeded);
                return RecordError::LimitExceeded;
            }
        }
        void *storage = message->trace->pool.alloc(allocation_charge, alignof(DataChunk));
        if (!storage) {
            mark_truncated(trace_data, 0, rendered_size, RecordError::NoMemory);
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
        if (trace_data.truncated) {
            core->on_trace_truncated(trace_data);
        }
        const TraceDisposition disposition = core->trace_disposition(trace_data.has_problem);
        if (disposition == TraceDisposition::Aggregate) {
            core->aggregate_trace(trace_data);
        } else if (disposition == TraceDisposition::Detailed || disposition == TraceDisposition::Problem) {
            auto encoded = core->encode(trace_data);
            if (encoded) {
                const SubmitResult submitted = core->submit_encoded(
                        std::move(*encoded),
                        disposition == TraceDisposition::Problem ? FramePriority::Problem : FramePriority::Normal);
                if (submitted == SubmitResult::Full && disposition == TraceDisposition::Detailed) {
                    core->aggregate_trace(trace_data);
                }
            } else {
                core->on_encode_failure(encoded.error());
                if (disposition == TraceDisposition::Detailed) {
                    core->aggregate_trace(trace_data);
                }
            }
        }
    }
    std::destroy_at(&trace_data);
    trace->data = nullptr;
    trace->pool.reset();
    if (!trace->public_handle_alive && trace->context_iteration_depth == 0) {
        delete trace;
    }
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

RecordError validate_trace_context(const RecordLimits &limits, const TraceContext &context) noexcept {
    if (context.message_id.empty() && (!context.root_message_id.empty() || !context.parent_message_id.empty())) {
        return RecordError::InvalidContext;
    }
    if (context.message_id.size() > limits.max_message_id_bytes ||
        context.root_message_id.size() > limits.max_message_id_bytes ||
        context.parent_message_id.size() > limits.max_message_id_bytes ||
        context.session_token.size() > limits.max_session_token_bytes) {
        return RecordError::LimitExceeded;
    }
    const auto valid_value = [](std::string_view value) noexcept {
        return std::all_of(value.begin(), value.end(), [](unsigned char byte) { return byte >= 0x21 && byte <= 0x7e; });
    };
    if (!valid_value(context.message_id) || !valid_value(context.root_message_id) ||
        !valid_value(context.parent_message_id) || !valid_value(context.session_token)) {
        return RecordError::InvalidContext;
    }
    return RecordError::None;
}

MessageTrace::~MessageTrace() {
    if (data) {
        std::destroy_at(data);
        data = nullptr;
    }
}

std::expected<MessageTrace *, RecordError> create_message_trace(RecordLimits limits, TraceContext context) noexcept {
    auto created = create_trace(limits, std::move(context));
    if (!created) {
        return std::unexpected(created.error());
    }
    (*created)->public_handle_alive = true;
    return *created;
}

void release_message_trace(MessageTrace *&trace_handle) noexcept {
    MessageTrace *trace = std::exchange(trace_handle, nullptr);
    if (!trace) {
        return;
    }
    if (trace->context_iteration_depth != 0) {
        FIBER_ASSERT(trace->public_handle_alive);
        trace->public_handle_alive = false;
        return;
    }
    if (!trace->data || !trace->data->root) {
        delete trace;
        return;
    }
    FIBER_ASSERT(on_owner_loop(*trace->data));
    FIBER_ASSERT(trace->public_handle_alive);
    trace->public_handle_alive = false;
}

RecordError put_context(MessageTrace &trace, std::string_view key, std::string_view value) noexcept {
    const RecordError access = validate_context_access(trace);
    if (access != RecordError::None) {
        return access;
    }

    MessageTraceData &data = *trace.data;
    if (key.empty()) {
        return RecordError::InvalidArgument;
    }
    if (key.size() > data.limits.max_context_key_bytes || value.size() > data.limits.max_context_value_bytes) {
        return RecordError::LimitExceeded;
    }

    const std::uint64_t hash = hash_context_key(key);
    ContextEntry *entry = find_context_entry(data.context, key, hash);
    if (entry) {
        if (entry->value() == value) {
            return RecordError::None;
        }
        if (value.size() <= entry->value_capacity) {
            if (!value.empty()) {
                std::copy(value.begin(), value.end(), entry->value_data);
            }
            entry->value_size = value.size();
            ++data.context.version;
            return RecordError::None;
        }
        if (!can_charge_context(data, value.size())) {
            return RecordError::LimitExceeded;
        }
        auto *replacement = static_cast<char *>(trace.pool.alloc(value.size(), alignof(char)));
        if (!replacement) {
            return RecordError::NoMemory;
        }
        std::copy(value.begin(), value.end(), replacement);
        entry->value_data = replacement;
        entry->value_size = value.size();
        entry->value_capacity = value.size();
        charge_context(data, value.size());
        ++data.context.version;
        return RecordError::None;
    }

    if (data.context.size >= data.limits.max_context_entries) {
        return RecordError::LimitExceeded;
    }
    const RecordError buckets = ensure_context_buckets(trace);
    if (buckets != RecordError::None) {
        return buckets;
    }

    std::size_t entry_bytes = 0;
    if (!checked_add(sizeof(ContextEntry), key.size(), entry_bytes) ||
        !checked_add(entry_bytes, value.size(), entry_bytes) || !can_charge_context(data, entry_bytes)) {
        return RecordError::LimitExceeded;
    }
    void *storage = trace.pool.alloc(entry_bytes, alignof(ContextEntry));
    if (!storage) {
        return RecordError::NoMemory;
    }

    entry = new (storage) ContextEntry;
    auto *text = reinterpret_cast<char *>(entry + 1);
    std::copy(key.begin(), key.end(), text);
    entry->key = {text, key.size()};
    text += key.size();
    if (!value.empty()) {
        std::copy(value.begin(), value.end(), text);
        entry->value_data = text;
    }
    entry->value_size = value.size();
    entry->value_capacity = value.size();
    entry->hash = hash;

    const std::size_t bucket = hash & (data.context.bucket_count - 1);
    entry->next_bucket = data.context.buckets[bucket];
    data.context.buckets[bucket] = entry;
    entry->prev_all = data.context.all_tail;
    if (data.context.all_tail) {
        data.context.all_tail->next_all = entry;
    } else {
        data.context.all_head = entry;
    }
    data.context.all_tail = entry;
    ++data.context.size;
    ++data.context.version;
    charge_context(data, entry_bytes);
    return RecordError::None;
}

std::expected<std::optional<std::string_view>, RecordError> get_context(const MessageTrace &trace,
                                                                        std::string_view key) noexcept {
    const RecordError access = validate_context_access(trace);
    if (access != RecordError::None) {
        return std::unexpected(access);
    }
    if (key.empty()) {
        return std::unexpected(RecordError::InvalidArgument);
    }
    const ContextEntry *entry = find_context_entry(trace.data->context, key, hash_context_key(key));
    if (!entry) {
        return std::optional<std::string_view>{};
    }
    return std::optional<std::string_view>{entry->value()};
}

std::expected<bool, RecordError> remove_context(MessageTrace &trace, std::string_view key) noexcept {
    const RecordError access = validate_context_access(trace);
    if (access != RecordError::None) {
        return std::unexpected(access);
    }
    if (key.empty()) {
        return std::unexpected(RecordError::InvalidArgument);
    }

    ContextTable &table = trace.data->context;
    if (!table.buckets) {
        return false;
    }
    const std::uint64_t hash = hash_context_key(key);
    ContextEntry *previous = nullptr;
    ContextEntry *entry = find_context_entry(table, key, hash, &previous);
    if (!entry) {
        return false;
    }

    const std::size_t bucket = hash & (table.bucket_count - 1);
    if (previous) {
        previous->next_bucket = entry->next_bucket;
    } else {
        table.buckets[bucket] = entry->next_bucket;
    }
    if (entry->prev_all) {
        entry->prev_all->next_all = entry->next_all;
    } else {
        table.all_head = entry->next_all;
    }
    if (entry->next_all) {
        entry->next_all->prev_all = entry->prev_all;
    } else {
        table.all_tail = entry->prev_all;
    }
    entry->next_bucket = nullptr;
    entry->next_all = nullptr;
    entry->prev_all = nullptr;
    --table.size;
    ++table.version;
    return true;
}

RecordError for_each_context(MessageTrace &trace, void *opaque, ContextVisitorFn visitor) noexcept {
    const RecordError access = validate_context_access(trace);
    if (access != RecordError::None) {
        return access;
    }
    if (!visitor) {
        return RecordError::InvalidArgument;
    }

    MessageTraceData *const data = trace.data;
    const std::uint64_t version = data->context.version;
    RecordError result = RecordError::None;
    ++trace.context_iteration_depth;
    for (const ContextEntry *entry = data->context.all_head; entry;) {
        const ContextEntry *next = entry->next_all;
        const bool continue_iteration = visitor(opaque, entry->key.view(), entry->value());
        if (trace.data != data) {
            result = RecordError::Completed;
            break;
        }
        if (data->context.version != version) {
            result = RecordError::InvalidArgument;
            break;
        }
        if (!continue_iteration) {
            break;
        }
        entry = next;
    }
    FIBER_ASSERT(trace.context_iteration_depth > 0);
    --trace.context_iteration_depth;

    const bool delete_trace =
            trace.context_iteration_depth == 0 && !trace.public_handle_alive && (!trace.data || !trace.data->root);
    if (delete_trace) {
        delete &trace;
    }
    return result;
}

std::expected<TransactionData *, RecordError> create_transaction_root(MessageTrace &trace, std::string_view type,
                                                                      std::string_view name) noexcept {
    return create_root<TransactionData>(trace, type, name);
}

std::expected<EventData *, RecordError> create_event_root(MessageTrace &trace, std::string_view type,
                                                          std::string_view name) noexcept {
    return create_root<EventData>(trace, type, name);
}

std::expected<TransactionData *, RecordError> create_transaction_root(std::string_view type, std::string_view name,
                                                                      RecordLimits limits,
                                                                      TraceContext context) noexcept {
    auto created_trace = create_trace(limits, std::move(context));
    if (!created_trace) {
        return std::unexpected(created_trace.error());
    }
    MessageTrace *trace = *created_trace;
    auto root = create_root<TransactionData>(*trace, type, name);
    if (!root) {
        delete trace;
        return std::unexpected(root.error());
    }
    return *root;
}

std::expected<EventData *, RecordError> create_event_root(std::string_view type, std::string_view name,
                                                          RecordLimits limits, TraceContext context) noexcept {
    auto created_trace = create_trace(limits, std::move(context));
    if (!created_trace) {
        return std::unexpected(created_trace.error());
    }
    MessageTrace *trace = *created_trace;
    auto root = create_root<EventData>(*trace, type, name);
    if (!root) {
        delete trace;
        return std::unexpected(root.error());
    }
    return *root;
}

std::expected<MetricMessageData *, RecordError> create_metric_root(std::string_view type, std::string_view name,
                                                                   RecordLimits limits, TraceContext context) noexcept {
    auto created_trace = create_trace(limits, std::move(context));
    if (!created_trace) {
        return std::unexpected(created_trace.error());
    }
    MessageTrace *trace = *created_trace;
    auto root = create_root<MetricMessageData>(*trace, type, name);
    if (!root) {
        delete trace;
        return std::unexpected(root.error());
    }
    return *root;
}

std::expected<HeartbeatData *, RecordError> create_heartbeat_root(std::string_view type, std::string_view name,
                                                                  RecordLimits limits, TraceContext context) noexcept {
    auto created_trace = create_trace(limits, std::move(context));
    if (!created_trace) {
        return std::unexpected(created_trace.error());
    }
    MessageTrace *trace = *created_trace;
    auto root = create_root<HeartbeatData>(*trace, type, name);
    if (!root) {
        delete trace;
        return std::unexpected(root.error());
    }
    return *root;
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
        mark_truncated(trace_data, 1, type.size() + name.size(), RecordError::LimitExceeded);
        return std::unexpected(RecordError::LimitExceeded);
    }

    const bool needs_chunk = parent.child_count % kChildrenPerChunk == 0;
    const std::size_t chunk_charge = needs_chunk ? sizeof(ChildrenChunk) : 0;
    std::size_t node_charge = 0;
    const RecordError validation = validate_message(trace_data, type, name, sizeof(Node), chunk_charge, node_charge);
    if (validation != RecordError::None) {
        mark_truncated(trace_data, 1, type.size() + name.size(), validation);
        return std::unexpected(validation);
    }

    Node *child = allocate_message<Node>(trace, type, name, node_charge);
    if (!child) {
        mark_truncated(trace_data, 1, type.size() + name.size(), RecordError::NoMemory);
        return std::unexpected(RecordError::NoMemory);
    }

    ChildrenChunk *new_chunk = nullptr;
    if (needs_chunk) {
        auto *storage = trace.pool.alloc<ChildrenChunk>();
        if (!storage) {
            trace_data.payload_bytes += node_charge;
            mark_truncated(trace_data, 1, type.size() + name.size(), RecordError::NoMemory);
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

std::expected<MetricMessageData *, RecordError> create_metric(TransactionData &parent, std::string_view type,
                                                              std::string_view name) noexcept {
    return create_child<MetricMessageData>(parent, type, name);
}

std::expected<HeartbeatData *, RecordError> create_heartbeat(TransactionData &parent, std::string_view type,
                                                             std::string_view name) noexcept {
    return create_child<HeartbeatData>(parent, type, name);
}

RecordError add_data(MessageData *message, std::string_view data) noexcept {
    return append_data(message, data, {}, false);
}

RecordError add_data(MessageData *message, std::string_view key, std::string_view value) noexcept {
    return append_data(message, key, value, true);
}

namespace {

RecordError set_message_text(MessageData *message, std::string_view value, std::size_t RecordLimits::*limit_field,
                             StringRef MessageData::*field) noexcept {
    const RecordError mutable_result = validate_mutation(message);
    if (mutable_result != RecordError::None) {
        return mutable_result;
    }
    MessageTraceData &trace_data = *message->trace->data;
    const std::size_t limit = trace_data.limits.*limit_field;
    if (value.size() > limit) {
        mark_truncated(trace_data, 0, value.size(), RecordError::LimitExceeded);
        return RecordError::LimitExceeded;
    }

    StringRef &current = message->*field;
    if (current.view() == value) {
        return RecordError::None;
    }
    if (value.empty()) {
        current = literal_ref("");
        return RecordError::None;
    }
    if (!can_charge(trace_data, value.size())) {
        mark_truncated(trace_data, 0, value.size(), RecordError::LimitExceeded);
        return RecordError::LimitExceeded;
    }
    StringRef copy = copy_string(*message->trace, value);
    if (!copy.data) {
        mark_truncated(trace_data, 0, value.size(), RecordError::NoMemory);
        return RecordError::NoMemory;
    }
    current = copy;
    trace_data.payload_bytes += value.size();
    return RecordError::None;
}

} // namespace

RecordError set_type(MessageData *message, std::string_view value) noexcept {
    return set_message_text(message, value, &RecordLimits::max_type_bytes, &MessageData::type);
}

RecordError set_name(MessageData *message, std::string_view value) noexcept {
    return set_message_text(message, value, &RecordLimits::max_name_bytes, &MessageData::name);
}

RecordError set_status(MessageData *message, std::string_view value) noexcept {
    const RecordError mutable_result = validate_mutation(message);
    if (mutable_result != RecordError::None) {
        return mutable_result;
    }
    MessageTraceData &trace_data = *message->trace->data;
    if (value.size() > trace_data.limits.max_status_bytes || !can_charge(trace_data, value.size())) {
        mark_truncated(trace_data, 0, value.size(), RecordError::LimitExceeded);
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
        mark_truncated(trace_data, 0, value.size(), RecordError::NoMemory);
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

RecordError complete_with_duration(TransactionData *&transaction, std::chrono::microseconds duration,
                                   std::string_view status_value, std::string_view data) noexcept {
    if (!transaction) {
        return RecordError::Completed;
    }
    if (duration.count() < 0) {
        return RecordError::InvalidArgument;
    }

    RecordError result = set_duration(transaction, duration);
    const auto wall_now = std::chrono::system_clock::now().time_since_epoch();
    const auto wall_millis = std::chrono::duration_cast<std::chrono::milliseconds>(wall_now).count();
    const auto duration_millis = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    const std::uint64_t completed_at = wall_millis <= 0 ? 0 : static_cast<std::uint64_t>(wall_millis);
    const std::uint64_t elapsed = duration_millis <= 0 ? 0 : static_cast<std::uint64_t>(duration_millis);
    const RecordError timestamp_result =
            set_timestamp(transaction, elapsed > completed_at ? 0 : completed_at - elapsed);
    if (result == RecordError::None) {
        result = timestamp_result;
    }
    if (status_value != status::Success) {
        const RecordError status_result = set_status(transaction, status_value);
        if (result == RecordError::None) {
            result = status_result;
        }
    }
    if (!data.empty()) {
        const RecordError data_result = add_data(transaction, data);
        if (result == RecordError::None) {
            result = data_result;
        }
    }
    const RecordError complete_result = complete(transaction);
    return result != RecordError::None ? result : complete_result;
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
