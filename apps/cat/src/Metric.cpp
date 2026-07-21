#include <fiber/cat/Metric.h>

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

#include <event/EventLoop.h>

namespace fiber::cat::detail {

struct MetricData {
    event::EventLoop *owner = nullptr;
    MetricKind kind = MetricKind::Count;
    std::int64_t quantity = 0;
    std::uint64_t duration_sum_millis = 0;
    std::size_t name_size = 0;

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
    ::operator delete(data_);
    data_ = std::exchange(other.data_, nullptr);
    return *this;
}

Metric::~Metric() { ::operator delete(data_); }

std::expected<Metric, RecordError> Metric::create_count(std::string_view name) noexcept {
    return create(MetricKind::Count, name);
}

std::expected<Metric, RecordError> Metric::create_duration(std::string_view name) noexcept {
    return create(MetricKind::Duration, name);
}

std::expected<Metric, RecordError> Metric::create(MetricKind kind_value, std::string_view name_value) noexcept {
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
    void *storage = ::operator new(sizeof(detail::MetricData) + name_value.size(), std::nothrow);
    if (!storage) {
        return std::unexpected(RecordError::NoMemory);
    }
    auto *data = new (storage) detail::MetricData{.owner = loop, .kind = kind_value, .name_size = name_value.size()};
    std::copy(name_value.begin(), name_value.end(), reinterpret_cast<char *>(data + 1));
    return Metric(data);
}

std::string_view Metric::name() const noexcept {
    return data_ ? std::string_view(data_->name_data(), data_->name_size) : std::string_view{};
}

MetricKind Metric::kind() const noexcept { return data_ ? data_->kind : MetricKind::Count; }

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
    data_->quantity = 0;
    data_->duration_sum_millis = 0;
    return RecordError::None;
}

} // namespace fiber::cat
