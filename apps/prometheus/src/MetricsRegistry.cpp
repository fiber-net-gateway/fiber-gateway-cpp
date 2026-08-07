#include "fiber/prometheus/MetricsRegistry.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

#include <fiber/common/Assert.h>
#include "PrometheusInternal.h"
#include "TextEncoder.h"

namespace fiber::prometheus {

namespace {

using fiber::common::IoErr;

bool valid_metric_name(std::string_view name) noexcept {
    if (name.empty()) {
        return false;
    }
    const auto valid_first = [](char ch) noexcept {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_' || ch == ':';
    };
    const auto valid_rest = [&](char ch) noexcept { return valid_first(ch) || (ch >= '0' && ch <= '9'); };
    return valid_first(name.front()) && std::all_of(name.begin() + 1, name.end(), valid_rest);
}

bool valid_label_name(std::string_view name) noexcept {
    if (name.empty()) {
        return false;
    }
    const auto valid_first = [](char ch) noexcept {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
    };
    const auto valid_rest = [&](char ch) noexcept { return valid_first(ch) || (ch >= '0' && ch <= '9'); };
    return valid_first(name.front()) && std::all_of(name.begin() + 1, name.end(), valid_rest);
}

std::size_t escaped_help_size(std::string_view value) noexcept {
    std::size_t size = value.size();
    for (char ch: value) {
        if (ch == '\\' || ch == '\n') {
            ++size;
        }
    }
    return size;
}

std::size_t escaped_label_size(std::string_view value) noexcept {
    std::size_t size = value.size();
    for (char ch: value) {
        if (ch == '\\' || ch == '"' || ch == '\n') {
            ++size;
        }
    }
    return size;
}

bool has_suffix(std::string_view value, std::string_view suffix) noexcept {
    return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
}

bool valid_reduction(GaugeReduction reduction) noexcept {
    return reduction == GaugeReduction::Sum || reduction == GaugeReduction::Min || reduction == GaugeReduction::Max;
}

bool valid_histogram_unit(HistogramUnit unit) noexcept {
    return unit == HistogramUnit::Raw || unit == HistogramUnit::Nanoseconds || unit == HistogramUnit::Microseconds ||
           unit == HistogramUnit::Milliseconds || unit == HistogramUnit::Seconds;
}

bool names_conflict(const detail::FamilySchema &left, const detail::FamilySchema &right) {
    const auto conflict_with = [](const detail::FamilySchema &family, std::string_view candidate) {
        if (family.name == candidate) {
            return true;
        }
        if (family.type != MetricType::Histogram) {
            return false;
        }
        return candidate == family.name + "_bucket" || candidate == family.name + "_sum" ||
               candidate == family.name + "_count";
    };

    if (conflict_with(left, right.name) || conflict_with(right, left.name)) {
        return true;
    }
    if (right.type == MetricType::Histogram &&
        (conflict_with(left, right.name + "_bucket") || conflict_with(left, right.name + "_sum") ||
         conflict_with(left, right.name + "_count"))) {
        return true;
    }
    if (left.type == MetricType::Histogram &&
        (conflict_with(right, left.name + "_bucket") || conflict_with(right, left.name + "_sum") ||
         conflict_with(right, left.name + "_count"))) {
        return true;
    }
    return false;
}

bool add_words(std::size_t &total, std::size_t count) noexcept {
    if (count > std::numeric_limits<std::size_t>::max() - total) {
        return false;
    }
    total += count;
    return true;
}

} // namespace

namespace detail {

ShardData::~ShardData() { reset(); }

bool ShardData::allocate(std::size_t word_count) noexcept {
    reset();
    if (word_count == 0) {
        return true;
    }
    if (word_count > std::numeric_limits<std::size_t>::max() / sizeof(std::uint64_t)) {
        return false;
    }
    const std::size_t value_bytes = word_count * sizeof(std::uint64_t);
    if (value_bytes > std::numeric_limits<std::size_t>::max() - (kCacheLineSize - 1)) {
        return false;
    }
    storage_bytes = (value_bytes + kCacheLineSize - 1) & ~(kCacheLineSize - 1);
    storage = static_cast<std::byte *>(::operator new(storage_bytes, std::align_val_t{kCacheLineSize}, std::nothrow));
    if (!storage) {
        storage_bytes = 0;
        return false;
    }
    std::memset(storage, 0, storage_bytes);
    return true;
}

void ShardData::reset() noexcept {
    if (storage) {
        ::operator delete(storage, std::align_val_t{kCacheLineSize});
    }
    storage = nullptr;
    storage_bytes = 0;
}

} // namespace detail

MetricsShard::MetricsShard(MetricsRegistry &registry, fiber::event::EventLoop &owner) :
    data_(std::make_unique<detail::ShardData>(registry, owner)) {}

MetricsShard::~MetricsShard() = default;

fiber::event::EventLoop &MetricsShard::owner_loop() const noexcept { return *data_->owner; }

fiber::common::IoResult<CounterRef> MetricsShard::counter(SeriesId id) noexcept {
    if (id.registry_ != data_->registry || !data_->registry->data_->frozen ||
        id.family_index_ >= data_->registry->data_->families.size()) {
        return std::unexpected(IoErr::Invalid);
    }
    auto &family = data_->registry->data_->families[id.family_index_];
    if (family.type != MetricType::Counter || id.series_index_ >= family.series.size()) {
        return std::unexpected(IoErr::Invalid);
    }
    return CounterRef(data_->words()[family.series[id.series_index_].word_offset]);
}

fiber::common::IoResult<GaugeRef> MetricsShard::gauge(SeriesId id) noexcept {
    if (id.registry_ != data_->registry || !data_->registry->data_->frozen ||
        id.family_index_ >= data_->registry->data_->families.size()) {
        return std::unexpected(IoErr::Invalid);
    }
    auto &family = data_->registry->data_->families[id.family_index_];
    if (family.type != MetricType::Gauge || id.series_index_ >= family.series.size()) {
        return std::unexpected(IoErr::Invalid);
    }
    auto *values = reinterpret_cast<std::int64_t *>(data_->storage);
    return GaugeRef(values[family.series[id.series_index_].word_offset]);
}

fiber::common::IoResult<HistogramRef> MetricsShard::histogram(SeriesId id) noexcept {
    if (id.registry_ != data_->registry || !data_->registry->data_->frozen ||
        id.family_index_ >= data_->registry->data_->families.size()) {
        return std::unexpected(IoErr::Invalid);
    }
    auto &family = data_->registry->data_->families[id.family_index_];
    if (family.type != MetricType::Histogram || id.series_index_ >= family.series.size()) {
        return std::unexpected(IoErr::Invalid);
    }
    std::uint64_t *values = data_->words() + family.series[id.series_index_].word_offset;
    const std::size_t bucket_count = family.upper_bounds.size();
    return HistogramRef(family.upper_bounds, std::span<std::uint64_t>(values, bucket_count), values[bucket_count],
                        values[bucket_count + 1]);
}

MetricsRegistry::MetricsRegistry(RegistryOptions options) : data_(std::make_unique<detail::RegistryData>(options)) {}

MetricsRegistry::~MetricsRegistry() {
    std::lock_guard guard(data_->collect_mutex);
    FIBER_ASSERT(!data_->collect_active);
}

fiber::common::IoResult<FamilyId> MetricsRegistry::register_counter(std::string_view name, std::string_view help,
                                                                    std::span<const std::string_view> label_names) {
    return register_family(MetricType::Counter, name, help, label_names, GaugeReduction::Sum, {}, HistogramUnit::Raw);
}

fiber::common::IoResult<FamilyId> MetricsRegistry::register_gauge(std::string_view name, std::string_view help,
                                                                  GaugeReduction reduction,
                                                                  std::span<const std::string_view> label_names) {
    return register_family(MetricType::Gauge, name, help, label_names, reduction, {}, HistogramUnit::Raw);
}

fiber::common::IoResult<FamilyId> MetricsRegistry::register_histogram(std::string_view name, std::string_view help,
                                                                      std::span<const std::uint64_t> upper_bounds,
                                                                      HistogramUnit unit,
                                                                      std::span<const std::string_view> label_names) {
    return register_family(MetricType::Histogram, name, help, label_names, GaugeReduction::Sum, upper_bounds, unit);
}

fiber::common::IoResult<FamilyId>
MetricsRegistry::register_family(MetricType type, std::string_view name, std::string_view help,
                                 std::span<const std::string_view> label_names, GaugeReduction reduction,
                                 std::span<const std::uint64_t> upper_bounds, HistogramUnit unit) {
    if (data_->frozen || !valid_metric_name(name) || escaped_help_size(help) > data_->options.max_help_bytes ||
        (type == MetricType::Counter && !has_suffix(name, "_total")) ||
        (type == MetricType::Gauge && !valid_reduction(reduction)) ||
        (type == MetricType::Histogram &&
         (!valid_histogram_unit(unit) || (has_suffix(name, "_seconds") != (unit != HistogramUnit::Raw)))) ||
        data_->families.size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(IoErr::Invalid);
    }
    if (type == MetricType::Histogram &&
        (upper_bounds.empty() || !std::is_sorted(upper_bounds.begin(), upper_bounds.end()) ||
         std::adjacent_find(upper_bounds.begin(), upper_bounds.end()) != upper_bounds.end())) {
        return std::unexpected(IoErr::Invalid);
    }

    detail::FamilySchema candidate;
    candidate.type = type;
    candidate.reduction = reduction;
    candidate.histogram_unit = unit;
    candidate.name = name;
    candidate.help = help;
    candidate.upper_bounds.assign(upper_bounds.begin(), upper_bounds.end());
    candidate.label_names.reserve(label_names.size());
    for (std::string_view label: label_names) {
        if (!valid_label_name(label) || (type == MetricType::Histogram && label == "le") ||
            std::find(candidate.label_names.begin(), candidate.label_names.end(), label) !=
                    candidate.label_names.end()) {
            return std::unexpected(IoErr::Invalid);
        }
        candidate.label_names.emplace_back(label);
    }
    if (std::any_of(data_->families.begin(), data_->families.end(),
                    [&](const detail::FamilySchema &family) { return names_conflict(family, candidate); })) {
        return std::unexpected(IoErr::Invalid);
    }

    data_->families.push_back(std::move(candidate));
    return FamilyId(this, static_cast<std::uint32_t>(data_->families.size() - 1));
}

fiber::common::IoResult<SeriesId> MetricsRegistry::register_series(FamilyId id,
                                                                   std::span<const std::string_view> label_values) {
    if (data_->frozen || id.registry_ != this || id.index_ >= data_->families.size()) {
        return std::unexpected(IoErr::Invalid);
    }
    auto &family = data_->families[id.index_];
    if (label_values.size() != family.label_names.size() ||
        family.series.size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(IoErr::Invalid);
    }

    detail::SeriesSchema candidate;
    candidate.label_values.reserve(label_values.size());
    for (std::string_view value: label_values) {
        if (escaped_label_size(value) > data_->options.max_label_value_bytes) {
            return std::unexpected(IoErr::Invalid);
        }
        candidate.label_values.emplace_back(value);
    }
    if (std::any_of(family.series.begin(), family.series.end(), [&](const detail::SeriesSchema &series) {
            return series.label_values == candidate.label_values;
        })) {
        return std::unexpected(IoErr::Invalid);
    }

    family.series.push_back(std::move(candidate));
    return SeriesId(this, id.index_, static_cast<std::uint32_t>(family.series.size() - 1));
}

fiber::common::IoResult<ShardId> MetricsRegistry::add_shard(fiber::event::EventLoop &owner) {
    if (data_->frozen || data_->shards.size() > std::numeric_limits<std::uint32_t>::max() ||
        std::any_of(data_->shards.begin(), data_->shards.end(),
                    [&](const std::unique_ptr<MetricsShard> &shard) { return &shard->owner_loop() == &owner; })) {
        return std::unexpected(IoErr::Invalid);
    }
    data_->shards.push_back(std::unique_ptr<MetricsShard>(new MetricsShard(*this, owner)));
    return ShardId(this, static_cast<std::uint32_t>(data_->shards.size() - 1));
}

fiber::common::IoResult<void> MetricsRegistry::freeze() {
    if (data_->frozen) {
        return std::unexpected(IoErr::Already);
    }

    std::size_t word_count = 0;
    std::size_t max_histogram_buckets = 0;
    for (auto &family: data_->families) {
        if (family.type == MetricType::Histogram) {
            max_histogram_buckets = std::max(max_histogram_buckets, family.upper_bounds.size());
        }
        const std::size_t series_words = family.type == MetricType::Histogram ? family.upper_bounds.size() + 2 : 1;
        for (auto &series: family.series) {
            series.word_offset = word_count;
            if (!add_words(word_count, series_words)) {
                return std::unexpected(IoErr::Invalid);
            }
        }
    }

    for (auto &shard: data_->shards) {
        if (!shard->data_->allocate(word_count)) {
            for (auto &allocated: data_->shards) {
                allocated->data_->reset();
            }
            return std::unexpected(IoErr::NoMem);
        }
    }
    data_->snapshots.clear();
    data_->snapshots.resize(data_->shards.size());
    for (auto &snapshot: data_->snapshots) {
        snapshot.resize(word_count);
    }
    data_->snapshot_requests.clear();
    data_->snapshot_requests.reserve(data_->shards.size());
    for (std::size_t index = 0; index < data_->shards.size(); ++index) {
        auto request = std::make_unique<detail::SnapshotRequest>();
        request->registry = this;
        request->shard_index = index;
        data_->snapshot_requests.push_back(std::move(request));
    }
    data_->histogram_scratch.resize(max_histogram_buckets);
    data_->word_count = word_count;
    data_->frozen = true;
    return {};
}

bool MetricsRegistry::frozen() const noexcept { return data_->frozen; }

std::size_t MetricsRegistry::family_count() const noexcept { return data_->families.size(); }

std::size_t MetricsRegistry::shard_count() const noexcept { return data_->shards.size(); }

MetricsShard *MetricsRegistry::shard(ShardId id) noexcept {
    if (id.registry_ != this || id.index_ >= data_->shards.size()) {
        return nullptr;
    }
    return data_->shards[id.index_].get();
}

const MetricsShard *MetricsRegistry::shard(ShardId id) const noexcept {
    if (id.registry_ != this || id.index_ >= data_->shards.size()) {
        return nullptr;
    }
    return data_->shards[id.index_].get();
}

MetricsRegistry::CollectToken::CollectToken(CollectToken &&other) noexcept :
    registry_(std::exchange(other.registry_, nullptr)) {}

MetricsRegistry::CollectToken &MetricsRegistry::CollectToken::operator=(CollectToken &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    if (registry_) {
        registry_->release_collect();
    }
    registry_ = std::exchange(other.registry_, nullptr);
    return *this;
}

MetricsRegistry::CollectToken::~CollectToken() {
    if (registry_) {
        registry_->release_collect();
    }
}

fiber::async::Task<fiber::common::IoResult<MetricsRegistry::CollectToken>> MetricsRegistry::prepare_collect() noexcept {
    fiber::event::EventLoop *collector = fiber::event::EventLoop::current_or_null();
    if (!collector || !collector->running()) {
        co_return std::unexpected(IoErr::Invalid);
    }

    std::size_t local_shard = data_->shards.size();
    std::size_t remote_count = 0;
    {
        std::lock_guard guard(data_->collect_mutex);
        if (!data_->frozen) {
            co_return std::unexpected(IoErr::Invalid);
        }
        if (!data_->accepting_collects) {
            co_return std::unexpected(IoErr::Canceled);
        }
        if (data_->collect_active) {
            co_return std::unexpected(IoErr::Busy);
        }
        for (std::size_t index = 0; index < data_->shards.size(); ++index) {
            fiber::event::EventLoop &owner = data_->shards[index]->owner_loop();
            if (!owner.running()) {
                co_return std::unexpected(IoErr::Invalid);
            }
            if (&owner == collector) {
                local_shard = index;
            } else {
                ++remote_count;
            }
        }

        data_->snapshot_wait.add(remote_count);
        data_->idle_wait.add();
        data_->pending_snapshots = remote_count;
        data_->collect_active = true;
        data_->collect_attached = true;
    }

    CollectToken token(*this);
    if (local_shard != data_->shards.size()) {
        copy_snapshot(local_shard);
    }
    for (std::size_t index = 0; index < data_->shards.size(); ++index) {
        fiber::event::EventLoop &owner = data_->shards[index]->owner_loop();
        if (&owner == collector) {
            continue;
        }
        auto &request = *data_->snapshot_requests[index];
        owner.post<detail::SnapshotRequest, &detail::SnapshotRequest::notify_entry, &MetricsRegistry::run_snapshot>(
                request);
    }

    co_await data_->snapshot_wait.join();
    co_return std::move(token);
}

void MetricsRegistry::copy_snapshot(std::size_t shard_index) noexcept {
    FIBER_ASSERT(shard_index < data_->shards.size());
    MetricsShard &source = *data_->shards[shard_index];
    FIBER_ASSERT(source.owner_loop().in_loop());
    if (data_->word_count != 0) {
        std::memcpy(data_->snapshots[shard_index].data(), source.data_->storage,
                    data_->word_count * sizeof(std::uint64_t));
    }
}

void MetricsRegistry::run_snapshot(detail::SnapshotRequest *request) noexcept {
    FIBER_ASSERT(request != nullptr);
    request->registry->copy_snapshot(request->shard_index);
    request->registry->data_->snapshot_wait.done();
    request->registry->snapshot_finished();
}

void MetricsRegistry::snapshot_finished() noexcept {
    bool became_idle = false;
    {
        std::lock_guard guard(data_->collect_mutex);
        FIBER_ASSERT(data_->collect_active);
        FIBER_ASSERT(data_->pending_snapshots > 0);
        --data_->pending_snapshots;
        if (data_->pending_snapshots == 0 && !data_->collect_attached) {
            data_->collect_active = false;
            became_idle = true;
        }
    }
    if (became_idle) {
        data_->idle_wait.done();
    }
}

void MetricsRegistry::release_collect() noexcept {
    bool became_idle = false;
    {
        std::lock_guard guard(data_->collect_mutex);
        FIBER_ASSERT(data_->collect_active);
        FIBER_ASSERT(data_->collect_attached);
        data_->collect_attached = false;
        if (data_->pending_snapshots == 0) {
            data_->collect_active = false;
            became_idle = true;
        }
    }
    if (became_idle) {
        data_->idle_wait.done();
    }
}

fiber::async::Task<fiber::common::IoResult<fiber::mem::IoBufChain>>
MetricsRegistry::collect_text(fiber::mem::IoBufNodePool &node_pool, CollectOptions options) noexcept {
    auto prepared = co_await prepare_collect();
    if (!prepared) {
        co_return std::unexpected(prepared.error());
    }
    co_return detail::encode_text_chain(*data_, node_pool, options);
}

fiber::async::Task<fiber::common::IoResult<std::size_t>>
MetricsRegistry::collect_text_into(fiber::mem::IoBuf &out, CollectOptions options) noexcept {
    if (!out) {
        co_return std::unexpected(IoErr::Invalid);
    }
    auto prepared = co_await prepare_collect();
    if (!prepared) {
        co_return std::unexpected(prepared.error());
    }
    co_return detail::encode_text_into(*data_, out, options);
}

void MetricsRegistry::stop_collecting() noexcept {
    std::lock_guard guard(data_->collect_mutex);
    data_->accepting_collects = false;
}

fiber::async::Task<void> MetricsRegistry::wait_for_idle() noexcept {
    co_await data_->idle_wait.join();
    co_return;
}

} // namespace fiber::prometheus
