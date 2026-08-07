#include <fiber/cat/Metric.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <new>
#include <utility>

#include <fiber/cat/CatClient.h>
#include <fiber/event/EventLoop.h>

#include "CatAggregation.h"
#include "CatClientCore.h"

namespace fiber::cat::detail {

struct MetricData {
    std::shared_ptr<CatClientCore> core;
    event::EventLoop *owner = nullptr;
    AggregationShard *aggregation_shard = nullptr;
    void *aggregate_entry = nullptr;
    MetricKind kind = MetricKind::Count;
    std::int64_t quantity = 0;
    std::uint64_t duration_sum_millis = 0;
    std::size_t name_size = 0;
    bool automatically_reported = false;

    [[nodiscard]] const char *name_data() const noexcept { return reinterpret_cast<const char *>(this + 1); }
};

} // namespace fiber::cat::detail

namespace fiber::cat {

namespace {

inline constexpr std::size_t kMaxMetricNameBytes = 1024;

bool on_owner_loop(const detail::MetricData &data) noexcept { return data.owner && data.owner->in_loop(); }

} // namespace

Metric::Metric(Metric &&other) noexcept : data_(std::exchange(other.data_, nullptr)) {}

Metric &Metric::operator=(Metric &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    if (data_) {
        std::destroy_at(data_);
        ::operator delete(data_);
    }
    data_ = std::exchange(other.data_, nullptr);
    return *this;
}

Metric::~Metric() {
    if (data_) {
        std::destroy_at(data_);
        ::operator delete(data_);
    }
}

std::expected<Metric, RecordError> Metric::create_count(std::string_view name) noexcept {
    return create(MetricKind::Count, name, nullptr);
}

std::expected<Metric, RecordError> Metric::create_duration(std::string_view name) noexcept {
    return create(MetricKind::Duration, name, nullptr);
}

std::expected<Metric, RecordError> Metric::create_count(CatClient &client, std::string_view name) noexcept {
    return create(MetricKind::Count, name, &client);
}

std::expected<Metric, RecordError> Metric::create_duration(CatClient &client, std::string_view name) noexcept {
    return create(MetricKind::Duration, name, &client);
}

std::expected<Metric, RecordError> Metric::create(MetricKind kind_value, std::string_view name_value,
                                                  CatClient *client) noexcept {
    event::EventLoop *loop = event::EventLoop::current_or_null();
    if (!loop) {
        return std::unexpected(RecordError::WrongEventLoop);
    }
    if (name_value.empty()) {
        return std::unexpected(RecordError::InvalidArgument);
    }
    if (name_value.size() > kMaxMetricNameBytes ||
        name_value.size() > std::numeric_limits<std::size_t>::max() - sizeof(detail::MetricData)) {
        return std::unexpected(RecordError::LimitExceeded);
    }
    std::shared_ptr<detail::CatClientCore> core;
    detail::AggregationShard *aggregation_shard = nullptr;
    void *aggregate_entry = nullptr;
    if (client) {
        core = client->core();
        if (!core || core->state() != CatClientState::Running) {
            return std::unexpected(RecordError::Completed);
        }
        aggregation_shard = core->aggregation_shard(*loop);
        if (!aggregation_shard) {
            return std::unexpected(RecordError::LimitExceeded);
        }
        auto registered = aggregation_shard->register_metric(kind_value, name_value);
        if (!registered) {
            return std::unexpected(registered.error());
        }
        aggregate_entry = *registered;
    }

    void *storage = ::operator new(sizeof(detail::MetricData) + name_value.size(), std::nothrow);
    if (!storage) {
        return std::unexpected(RecordError::NoMemory);
    }
    auto *data = new (storage) detail::MetricData{
            .core = std::move(core),
            .owner = loop,
            .aggregation_shard = aggregation_shard,
            .aggregate_entry = aggregate_entry,
            .kind = kind_value,
            .name_size = name_value.size(),
            .automatically_reported = client != nullptr,
    };
    std::copy(name_value.begin(), name_value.end(), reinterpret_cast<char *>(data + 1));
    return Metric(data);
}

std::string_view Metric::name() const noexcept {
    return data_ ? std::string_view(data_->name_data(), data_->name_size) : std::string_view{};
}

MetricKind Metric::kind() const noexcept { return data_ ? data_->kind : MetricKind::Count; }

bool Metric::automatically_reported() const noexcept { return data_ && data_->automatically_reported; }

RecordError Metric::record_count(std::int64_t quantity) noexcept {
    if (!data_) {
        return RecordError::InvalidArgument;
    }
    if (!on_owner_loop(*data_)) {
        return RecordError::WrongEventLoop;
    }
    if (data_->kind != MetricKind::Count) {
        return RecordError::WrongMetricKind;
    }
    if (data_->automatically_reported) {
        const RecordError result = data_->aggregation_shard->record_metric_count(data_->aggregate_entry, quantity);
        data_->core->on_metric_observation(result);
        return result;
    }
    if ((quantity > 0 && data_->quantity > std::numeric_limits<std::int64_t>::max() - quantity) ||
        (quantity < 0 && data_->quantity < std::numeric_limits<std::int64_t>::min() - quantity)) {
        return RecordError::LimitExceeded;
    }
    data_->quantity += quantity;
    return RecordError::None;
}

RecordError Metric::record_duration(std::chrono::milliseconds duration) noexcept {
    if (!data_) {
        return RecordError::InvalidArgument;
    }
    if (!on_owner_loop(*data_)) {
        return RecordError::WrongEventLoop;
    }
    if (data_->kind != MetricKind::Duration) {
        return RecordError::WrongMetricKind;
    }
    if (data_->automatically_reported) {
        const RecordError result = data_->aggregation_shard->record_metric_duration(data_->aggregate_entry, duration);
        data_->core->on_metric_observation(result);
        return result;
    }
    if (duration.count() < 0 || data_->quantity == std::numeric_limits<std::int64_t>::max()) {
        return RecordError::LimitExceeded;
    }
    const auto value = static_cast<std::uint64_t>(duration.count());
    if (value > std::numeric_limits<std::uint64_t>::max() - data_->duration_sum_millis) {
        return RecordError::LimitExceeded;
    }
    ++data_->quantity;
    data_->duration_sum_millis += value;
    return RecordError::None;
}

std::expected<MetricSnapshot, RecordError> Metric::snapshot() const noexcept {
    if (!data_) {
        return std::unexpected(RecordError::InvalidArgument);
    }
    if (!on_owner_loop(*data_)) {
        return std::unexpected(RecordError::WrongEventLoop);
    }
    if (data_->automatically_reported) {
        return std::unexpected(RecordError::InvalidArgument);
    }
    return MetricSnapshot{
            .name = name(),
            .kind = data_->kind,
            .quantity = data_->quantity,
            .duration_sum_millis = data_->duration_sum_millis,
    };
}

std::expected<MetricSnapshot, RecordError> Metric::snapshot_and_reset() noexcept {
    auto result = snapshot();
    if (!result) {
        return result;
    }
    data_->quantity = 0;
    data_->duration_sum_millis = 0;
    return result;
}

RecordError Metric::reset() noexcept {
    if (!data_) {
        return RecordError::InvalidArgument;
    }
    if (!on_owner_loop(*data_)) {
        return RecordError::WrongEventLoop;
    }
    if (data_->automatically_reported) {
        return RecordError::InvalidArgument;
    }
    data_->quantity = 0;
    data_->duration_sum_millis = 0;
    return RecordError::None;
}

} // namespace fiber::cat
