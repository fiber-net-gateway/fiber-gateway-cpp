#include "CatAggregation.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <limits>
#include <new>
#include <utility>

#include <fiber/cat/Status.h>
#include <fiber/common/Assert.h>

#include "CatClientCore.h"
#include "CatEncoder.h"
#include "CatInternal.h"

namespace fiber::cat::detail {

namespace {

inline constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
inline constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
inline constexpr std::size_t kAggregateDataCapacity = 4096;

std::uint64_t hash_key(AggregateKind kind, std::string_view type, std::string_view name) noexcept {
    std::uint64_t hash = kFnvOffsetBasis;
    const auto add = [&](unsigned char value) {
        hash ^= value;
        hash *= kFnvPrime;
    };
    add(static_cast<unsigned char>(kind));
    for (const unsigned char value: type) {
        add(value);
    }
    add(0xff);
    for (const unsigned char value: name) {
        add(value);
    }
    return hash;
}

std::size_t bucket_count_for(std::size_t max_keys) noexcept {
    std::size_t result = 8;
    while (result < max_keys && result <= std::numeric_limits<std::size_t>::max() / 2) {
        result *= 2;
    }
    return result;
}

std::uint64_t duration_bucket(std::uint64_t duration) noexcept {
    if (duration < 1) {
        return 1;
    }
    if (duration < 20) {
        return duration;
    }
    if (duration < 200) {
        return duration - duration % 5;
    }
    if (duration < 500) {
        return duration - duration % 20;
    }
    if (duration < 2000) {
        return duration - duration % 50;
    }
    return duration - duration % 200;
}

bool checked_add(std::size_t lhs, std::size_t rhs, std::size_t &result) noexcept {
    if (lhs > std::numeric_limits<std::size_t>::max() - rhs) {
        return false;
    }
    result = lhs + rhs;
    return true;
}

class TextWriter {
public:
    explicit TextWriter(std::array<char, kAggregateDataCapacity> &storage) noexcept : storage_(storage) {}

    bool text(std::string_view value) noexcept {
        if (value.size() > storage_.size() - size_) {
            return false;
        }
        std::copy(value.begin(), value.end(), storage_.data() + size_);
        size_ += value.size();
        return true;
    }

    bool number(std::uint64_t value) noexcept {
        auto result = std::to_chars(storage_.data() + size_, storage_.data() + storage_.size(), value);
        if (result.ec != std::errc{}) {
            return false;
        }
        size_ = static_cast<std::size_t>(result.ptr - storage_.data());
        return true;
    }

    bool signed_number(std::int64_t value) noexcept {
        auto result = std::to_chars(storage_.data() + size_, storage_.data() + storage_.size(), value);
        if (result.ec != std::errc{}) {
            return false;
        }
        size_ = static_cast<std::size_t>(result.ptr - storage_.data());
        return true;
    }

    [[nodiscard]] std::string_view view() const noexcept { return {storage_.data(), size_}; }

private:
    std::array<char, kAggregateDataCapacity> &storage_;
    std::size_t size_ = 0;
};

} // namespace

struct AggregationShard::DurationBucket {
    DurationBucket *next = nullptr;
    std::uint64_t duration_millis = 0;
    std::uint64_t count = 0;
};

struct AggregationShard::Entry {
    Entry *next_bucket = nullptr;
    Entry *next_all = nullptr;
    std::uint64_t hash = 0;
    std::string_view type;
    std::string_view name;
    AggregateKind kind = AggregateKind::Event;
    std::uint64_t count = 0;
    std::uint64_t error_count = 0;
    std::uint64_t duration_sum_millis = 0;
    DurationBucket *durations = nullptr;
    std::size_t duration_bucket_count = 0;
    std::int64_t metric_quantity = 0;
    std::uint64_t metric_duration_sum_millis = 0;
    bool metric_dirty = false;
};

struct AggregationShard::FlushRequest {
    std::shared_ptr<CatClientCore> core;
    AggregationShard *shard = nullptr;
    event::EventLoop::NotifyEntry notify_entry{};
};

AggregationShard *AggregationShard::create(event::EventLoop &owner, std::size_t max_keys, std::size_t max_key_bytes,
                                           std::size_t max_bytes, std::size_t max_duration_buckets) noexcept {
    if (max_keys == 0 || max_key_bytes == 0 || max_bytes < sizeof(Entry) || max_duration_buckets == 0) {
        return nullptr;
    }
    const std::size_t bucket_count = bucket_count_for(max_keys);
    if (bucket_count < max_keys || bucket_count > std::numeric_limits<std::size_t>::max() / sizeof(Entry *)) {
        return nullptr;
    }
    auto **buckets = new (std::nothrow) Entry *[bucket_count] {};
    if (!buckets) {
        return nullptr;
    }
    auto *shard = new (std::nothrow)
            AggregationShard(owner, max_keys, max_key_bytes, max_bytes, max_duration_buckets, buckets, bucket_count);
    if (!shard) {
        delete[] buckets;
    }
    return shard;
}

AggregationShard::AggregationShard(event::EventLoop &owner, std::size_t max_keys, std::size_t max_key_bytes,
                                   std::size_t max_bytes, std::size_t max_duration_buckets, Entry **buckets,
                                   std::size_t bucket_count) noexcept :
    owner_(&owner), buckets_(buckets), bucket_count_(bucket_count), max_keys_(max_keys), max_key_bytes_(max_key_bytes),
    max_bytes_(max_bytes), max_duration_buckets_(max_duration_buckets) {}

AggregationShard::~AggregationShard() { delete[] buckets_; }

std::size_t AggregationShard::aggregate(const MessageTraceData &trace) noexcept {
    FIBER_ASSERT(owner_->in_loop());
    if (!trace.root || trace.open_message_count != 0) {
        return trace.message_count == 0 ? 0 : trace.message_count;
    }
    return aggregate_message(*trace.root);
}

std::expected<void *, RecordError> AggregationShard::register_metric(MetricKind kind, std::string_view name) noexcept {
    if (!owner_->in_loop()) {
        return std::unexpected(RecordError::WrongEventLoop);
    }
    if (name.empty()) {
        return std::unexpected(RecordError::InvalidArgument);
    }
    std::size_t dropped = 0;
    Entry *entry = find_or_create(
            kind == MetricKind::Count ? AggregateKind::MetricCount : AggregateKind::MetricDuration, {}, name, dropped);
    if (!entry) {
        return std::unexpected(RecordError::LimitExceeded);
    }
    return static_cast<void *>(entry);
}

RecordError AggregationShard::record_metric_count(void *handle, std::int64_t quantity) noexcept {
    if (!owner_->in_loop()) {
        return RecordError::WrongEventLoop;
    }
    auto *entry = static_cast<Entry *>(handle);
    if (!entry || entry->kind != AggregateKind::MetricCount) {
        return RecordError::WrongMetricKind;
    }
    if ((quantity > 0 && entry->metric_quantity > std::numeric_limits<std::int64_t>::max() - quantity) ||
        (quantity < 0 && entry->metric_quantity < std::numeric_limits<std::int64_t>::min() - quantity)) {
        return RecordError::LimitExceeded;
    }
    entry->metric_quantity += quantity;
    entry->metric_dirty = true;
    return RecordError::None;
}

RecordError AggregationShard::record_metric_duration(void *handle, std::chrono::milliseconds duration) noexcept {
    if (!owner_->in_loop()) {
        return RecordError::WrongEventLoop;
    }
    auto *entry = static_cast<Entry *>(handle);
    if (!entry || entry->kind != AggregateKind::MetricDuration) {
        return RecordError::WrongMetricKind;
    }
    if (duration.count() < 0 || entry->metric_quantity == std::numeric_limits<std::int64_t>::max()) {
        return RecordError::LimitExceeded;
    }
    const auto value = static_cast<std::uint64_t>(duration.count());
    if (value > std::numeric_limits<std::uint64_t>::max() - entry->metric_duration_sum_millis) {
        return RecordError::LimitExceeded;
    }
    ++entry->metric_quantity;
    entry->metric_duration_sum_millis += value;
    entry->metric_dirty = true;
    return RecordError::None;
}

std::size_t AggregationShard::aggregate_message(const MessageData &message) noexcept {
    std::size_t dropped = 0;
    const AggregateKind kind =
            message.kind == MessageKind::Transaction ? AggregateKind::Transaction : AggregateKind::Event;
    Entry *entry = find_or_create(kind, message.type, message.name, dropped);
    if (entry) {
        if (entry->count == std::numeric_limits<std::uint64_t>::max()) {
            ++dropped;
        } else {
            ++entry->count;
            if (message.status != status::Success) {
                if (entry->error_count == std::numeric_limits<std::uint64_t>::max()) {
                    ++dropped;
                } else {
                    ++entry->error_count;
                }
            }
            if (kind == AggregateKind::Transaction) {
                const auto &transaction = static_cast<const TransactionData &>(message);
                const std::uint64_t duration =
                        transaction.duration.count() <= 0
                                ? 0
                                : static_cast<std::uint64_t>(transaction.duration.count()) / 1000;
                if (duration > std::numeric_limits<std::uint64_t>::max() - entry->duration_sum_millis) {
                    ++dropped;
                } else {
                    entry->duration_sum_millis += duration;
                }
                if (!add_duration(*entry, duration)) {
                    ++dropped;
                }
            }
        }
    }

    if (message.kind != MessageKind::Transaction) {
        return dropped;
    }
    const auto &transaction = static_cast<const TransactionData &>(message);
    std::size_t visited = 0;
    for (const ChildrenChunk *chunk = transaction.children_head; chunk; chunk = chunk->next) {
        const std::size_t count = std::min(kChildrenPerChunk, transaction.child_count - visited);
        for (std::size_t index = 0; index < count; ++index) {
            dropped += aggregate_message(*chunk->children[index]);
        }
        visited += count;
    }
    return dropped;
}

AggregationShard::Entry *AggregationShard::find_or_create(AggregateKind kind, std::string_view type,
                                                          std::string_view name, std::size_t &dropped) noexcept {
    const std::uint64_t hash = hash_key(kind, type, name);
    Entry *entry = buckets_[hash & (bucket_count_ - 1)];
    while (entry) {
        if (entry->hash == hash && entry->kind == kind && entry->type == type && entry->name == name) {
            return entry;
        }
        entry = entry->next_bucket;
    }

    std::size_t text_bytes = 0;
    std::size_t charge = 0;
    if (key_count_ >= max_keys_ || type.size() > max_key_bytes_ || name.size() > max_key_bytes_ ||
        !checked_add(type.size(), name.size(), text_bytes) || !checked_add(sizeof(Entry), text_bytes, charge) ||
        allocated_bytes_ > max_bytes_ || charge > max_bytes_ - allocated_bytes_) {
        ++dropped;
        return nullptr;
    }
    void *storage = pool_.alloc(charge, alignof(Entry));
    if (!storage) {
        ++dropped;
        return nullptr;
    }
    entry = new (storage) Entry;
    char *text = reinterpret_cast<char *>(entry + 1);
    std::copy(type.begin(), type.end(), text);
    entry->type = {text, type.size()};
    text += type.size();
    std::copy(name.begin(), name.end(), text);
    entry->name = {text, name.size()};
    entry->kind = kind;
    entry->hash = hash;
    const std::size_t bucket = hash & (bucket_count_ - 1);
    entry->next_bucket = buckets_[bucket];
    buckets_[bucket] = entry;
    if (all_tail_) {
        all_tail_->next_all = entry;
    } else {
        all_head_ = entry;
    }
    all_tail_ = entry;
    ++key_count_;
    allocated_bytes_ += charge;
    return entry;
}

bool AggregationShard::add_duration(Entry &entry, std::uint64_t duration_millis) noexcept {
    const std::uint64_t key = duration_bucket(duration_millis);
    DurationBucket **position = &entry.durations;
    while (*position && (*position)->duration_millis < key) {
        position = &(*position)->next;
    }
    if (*position && (*position)->duration_millis == key) {
        if ((*position)->count == std::numeric_limits<std::uint64_t>::max()) {
            return false;
        }
        ++(*position)->count;
        return true;
    }
    if (entry.duration_bucket_count >= max_duration_buckets_ || allocated_bytes_ > max_bytes_ ||
        sizeof(DurationBucket) > max_bytes_ - allocated_bytes_) {
        return false;
    }
    auto *storage = pool_.alloc<DurationBucket>();
    if (!storage) {
        return false;
    }
    auto *bucket = new (storage) DurationBucket{.next = *position, .duration_millis = key, .count = 1};
    *position = bucket;
    ++entry.duration_bucket_count;
    allocated_bytes_ += sizeof(DurationBucket);
    return true;
}

void AggregationShard::request_flush(std::shared_ptr<CatClientCore> core) noexcept {
    if (!core || core->state() != CatClientState::Running) {
        return;
    }
    if (owner_->in_loop()) {
        flush(*core);
        return;
    }
    auto *request = new (std::nothrow) FlushRequest{.core = std::move(core), .shard = this};
    if (!request) {
        return;
    }
    owner_->post<FlushRequest, &FlushRequest::notify_entry, &AggregationShard::on_flush_notify>(*request);
}

void AggregationShard::on_flush_notify(FlushRequest *request) noexcept {
    FIBER_ASSERT(request);
    FIBER_ASSERT(request->shard);
    if (request->core && request->core->state() == CatClientState::Running) {
        request->shard->flush(*request->core);
    }
    delete request;
}

void AggregationShard::flush(CatClientCore &core) noexcept {
    FIBER_ASSERT(owner_->in_loop());
    if (flush_kind(core, AggregateKind::Transaction)) {
        reset_kind(AggregateKind::Transaction);
    }
    if (flush_kind(core, AggregateKind::Event)) {
        reset_kind(AggregateKind::Event);
    }
    if (flush_metrics(core)) {
        reset_kind(AggregateKind::MetricCount);
        reset_kind(AggregateKind::MetricDuration);
    }
}

bool AggregationShard::flush_kind(CatClientCore &core, AggregateKind kind) noexcept {
    std::size_t count = 0;
    for (const Entry *entry = all_head_; entry; entry = entry->next_all) {
        if (entry->kind == kind && entry->count != 0) {
            ++count;
        }
    }
    if (count == 0) {
        return false;
    }

    auto id = core.create_message_id();
    if (!id) {
        core.on_aggregate_encode_failure();
        return false;
    }
    RecordLimits limits;
    limits.max_messages = count + 1;
    limits.max_children_per_transaction = count;
    limits.max_type_bytes = max_key_bytes_;
    limits.max_name_bytes = max_key_bytes_;
    limits.max_data_bytes_per_message = kAggregateDataCapacity;
    limits.max_tree_bytes = std::max<std::size_t>(max_bytes_, 64 * 1024);
    TraceContext context{.propagation_context = {.message_id = id->view()}};
    mem::BufPool tree_pool;
    auto root_created = create_transaction_root(
            tree_pool, "System", kind == AggregateKind::Transaction ? "TransactionAggregator" : "EventAggregator",
            limits, std::move(context));
    if (!root_created) {
        core.on_aggregate_encode_failure();
        return false;
    }
    TransactionData *root = *root_created;
    MessageTrace *trace = root->trace;
    bool failed = false;
    for (const Entry *entry = all_head_; entry && !failed; entry = entry->next_all) {
        if (entry->kind != kind || entry->count == 0) {
            continue;
        }
        std::array<char, kAggregateDataCapacity> storage{};
        TextWriter writer(storage);
        if (kind == AggregateKind::Transaction) {
            if (!writer.text("@") || !writer.number(entry->count) || !writer.text(";") ||
                !writer.number(entry->error_count) || !writer.text(";") || !writer.number(entry->duration_sum_millis) ||
                !writer.text(";")) {
                failed = true;
                break;
            }
            bool first = true;
            for (const DurationBucket *bucket = entry->durations; bucket; bucket = bucket->next) {
                if (bucket->count == 0) {
                    continue;
                }
                if ((!first && !writer.text("|")) || !writer.number(bucket->duration_millis) || !writer.text(",") ||
                    !writer.number(bucket->count)) {
                    failed = true;
                    break;
                }
                first = false;
            }
            if (failed) {
                break;
            }
            auto child_created = create_transaction(*root, entry->type, entry->name);
            if (!child_created) {
                failed = true;
                break;
            }
            TransactionData *child = *child_created;
            if (add_data(child, writer.view()) != RecordError::None) {
                failed = true;
                break;
            }
            child->duration = std::chrono::microseconds::zero();
            child->explicit_duration = true;
            child->completed = true;
        } else {
            if (!writer.text("@") || !writer.number(entry->count) || !writer.text(";") ||
                !writer.number(entry->error_count)) {
                failed = true;
                break;
            }
            auto child_created = create_event(*root, entry->type, entry->name);
            if (!child_created) {
                failed = true;
                break;
            }
            EventData *child = *child_created;
            if (add_data(child, writer.view()) != RecordError::None) {
                failed = true;
                break;
            }
            child->completed = true;
        }
    }
    root->duration = std::chrono::microseconds::zero();
    root->explicit_duration = true;
    root->completed = true;
    trace->data->open_message_count = 0;

    bool submitted = false;
    if (!failed) {
        auto encoded = core.encode(*trace->data);
        if (encoded) {
            submitted = core.submit_aggregate(std::move(*encoded));
        } else {
            core.on_aggregate_encode_failure();
        }
    } else {
        core.on_aggregate_encode_failure();
    }
    discard_message_trace(trace);
    return submitted;
}

bool AggregationShard::flush_metrics(CatClientCore &core) noexcept {
    std::size_t count = 0;
    for (const Entry *entry = all_head_; entry; entry = entry->next_all) {
        if ((entry->kind == AggregateKind::MetricCount || entry->kind == AggregateKind::MetricDuration) &&
            entry->metric_dirty) {
            ++count;
        }
    }
    if (count == 0) {
        return false;
    }

    auto id = core.create_message_id();
    if (!id) {
        core.on_aggregate_encode_failure();
        return false;
    }
    RecordLimits limits;
    limits.max_messages = count + 1;
    limits.max_children_per_transaction = count;
    limits.max_type_bytes = max_key_bytes_;
    limits.max_name_bytes = max_key_bytes_;
    limits.max_status_bytes = 16;
    limits.max_data_bytes_per_message = kAggregateDataCapacity;
    limits.max_tree_bytes = std::max<std::size_t>(max_bytes_, 64 * 1024);
    mem::BufPool tree_pool;
    auto root_created = create_transaction_root(tree_pool, "System", "MetricAggregator", limits,
                                                {.propagation_context = {.message_id = id->view()}});
    if (!root_created) {
        core.on_aggregate_encode_failure();
        return false;
    }
    TransactionData *root = *root_created;
    MessageTrace *trace = root->trace;
    bool failed = false;
    for (const Entry *entry = all_head_; entry && !failed; entry = entry->next_all) {
        if ((entry->kind != AggregateKind::MetricCount && entry->kind != AggregateKind::MetricDuration) ||
            !entry->metric_dirty) {
            continue;
        }
        std::array<char, kAggregateDataCapacity> storage{};
        TextWriter writer(storage);
        if (entry->kind == AggregateKind::MetricCount) {
            failed = !writer.signed_number(entry->metric_quantity);
        } else {
            failed = !writer.signed_number(entry->metric_quantity) || !writer.text(",") ||
                     !writer.number(entry->metric_duration_sum_millis);
        }
        if (failed) {
            break;
        }
        auto metric_created = create_metric(*root, {}, entry->name);
        if (!metric_created) {
            failed = true;
            break;
        }
        MetricMessageData *metric = *metric_created;
        const std::string_view metric_status =
                entry->kind == AggregateKind::MetricCount ? std::string_view("C") : std::string_view("S,C");
        if (set_status(metric, metric_status) != RecordError::None ||
            add_data(metric, writer.view()) != RecordError::None) {
            failed = true;
            break;
        }
        metric->completed = true;
    }
    root->duration = std::chrono::microseconds::zero();
    root->explicit_duration = true;
    root->completed = true;
    trace->data->open_message_count = 0;

    bool submitted = false;
    if (!failed) {
        auto encoded = core.encode(*trace->data);
        if (encoded) {
            submitted = core.submit_metric_aggregate(std::move(*encoded));
        } else {
            core.on_aggregate_encode_failure();
        }
    } else {
        core.on_aggregate_encode_failure();
    }
    discard_message_trace(trace);
    return submitted;
}

void AggregationShard::reset_kind(AggregateKind kind) noexcept {
    for (Entry *entry = all_head_; entry; entry = entry->next_all) {
        if (entry->kind != kind) {
            continue;
        }
        if (kind == AggregateKind::MetricCount || kind == AggregateKind::MetricDuration) {
            entry->metric_quantity = 0;
            entry->metric_duration_sum_millis = 0;
            entry->metric_dirty = false;
            continue;
        }
        entry->count = 0;
        entry->error_count = 0;
        entry->duration_sum_millis = 0;
        for (DurationBucket *bucket = entry->durations; bucket; bucket = bucket->next) {
            bucket->count = 0;
        }
    }
}

void AggregationShard::discard_pending(CatClientCore &core) noexcept {
    FIBER_ASSERT(owner_->in_loop());
    bool transaction_pending = false;
    bool event_pending = false;
    bool metric_pending = false;
    for (const Entry *entry = all_head_; entry; entry = entry->next_all) {
        transaction_pending = transaction_pending || (entry->kind == AggregateKind::Transaction && entry->count != 0);
        event_pending = event_pending || (entry->kind == AggregateKind::Event && entry->count != 0);
        metric_pending = metric_pending ||
                         ((entry->kind == AggregateKind::MetricCount || entry->kind == AggregateKind::MetricDuration) &&
                          entry->metric_dirty);
    }
    if (transaction_pending) {
        core.on_aggregate_drop();
        reset_kind(AggregateKind::Transaction);
    }
    if (event_pending) {
        core.on_aggregate_drop();
        reset_kind(AggregateKind::Event);
    }
    if (metric_pending) {
        core.on_metric_drop();
        reset_kind(AggregateKind::MetricCount);
        reset_kind(AggregateKind::MetricDuration);
    }
}

void AggregationShard::for_each(void *opaque, Visitor visitor) const noexcept {
    FIBER_ASSERT(owner_->in_loop());
    if (!visitor) {
        return;
    }
    for (const Entry *entry = all_head_; entry; entry = entry->next_all) {
        if (!visitor(opaque, AggregateValue{
                                     .kind = entry->kind,
                                     .type = entry->type,
                                     .name = entry->name,
                                     .count = entry->count,
                                     .error_count = entry->error_count,
                                     .duration_sum_millis = entry->duration_sum_millis,
                             })) {
            return;
        }
    }
}

} // namespace fiber::cat::detail
