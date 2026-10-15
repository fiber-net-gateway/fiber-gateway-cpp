#include "DynamicSeriesStore.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <malloc.h>
#include <memory>

#include <fiber/common/Assert.h>
#include "PrometheusInternal.h"
#include "fiber/prometheus/DynamicCounter.h"
#include "fiber/prometheus/DynamicHistogram.h"

namespace fiber::prometheus::detail {
namespace {
using fiber::common::IoErr;
using fiber::common::IoResult;

thread_local std::ptrdiff_t allocation_countdown = -1;
thread_local bool hash_collisions = false;
// Include a conservative two-word allocator block header as well as the
// allocator's actual usable capacity (including size-class rounding).
constexpr std::size_t kAllocationHeader = 2 * sizeof(std::size_t);

bool checked_add(std::size_t &total, std::size_t count) noexcept {
    if (count > std::numeric_limits<std::size_t>::max() - total) {
        return false;
    }
    total += count;
    return true;
}
bool checked_product(std::size_t &total, std::size_t count, std::size_t width) noexcept {
    return (width == 0 || count <= std::numeric_limits<std::size_t>::max() / width) &&
           checked_add(total, count * width);
}
std::uint64_t label_hash(std::span<const std::string_view> labels) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (std::string_view label: labels) {
        std::size_t length = label.size();
        for (std::size_t i = 0; i < sizeof(length); ++i) {
            hash = (hash ^ (length & 255)) * 1099511628211ULL;
            length >>= 8;
        }
        for (unsigned char byte: label) {
            hash = (hash ^ byte) * 1099511628211ULL;
        }
    }
    return hash_collisions ? 0 : hash;
}
bool fits(std::size_t current, std::size_t extra, std::size_t budget) noexcept {
    return extra <= std::numeric_limits<std::size_t>::max() - current &&
           (budget == 0 || (current <= budget && extra <= budget - current));
}
bool fits_allocation(std::size_t current, std::size_t bytes, std::size_t budget) noexcept {
    return checked_add(bytes, kAllocationHeader) && fits(current, bytes, budget);
}
} // namespace

void force_dynamic_hash_collisions(bool enabled) noexcept { hash_collisions = enabled; }
void fail_dynamic_allocation_after(std::ptrdiff_t successful_allocations) noexcept {
    allocation_countdown = successful_allocations;
}
void *dynamic_allocate(std::size_t bytes) noexcept {
    if (allocation_countdown == 0) {
        return nullptr;
    }
    if (allocation_countdown > 0) {
        --allocation_countdown;
    }
    return std::malloc(bytes);
}
void dynamic_deallocate(void *memory) noexcept { std::free(memory); }
std::size_t dynamic_allocation_bytes(const void *memory) noexcept {
    return memory ? malloc_usable_size(const_cast<void *>(memory)) + kAllocationHeader : 0;
}

DynamicSnapshot::~DynamicSnapshot() { dynamic_deallocate(memory_); }
IoResult<void> DynamicSnapshot::reserve(std::size_t bytes, std::size_t &total, std::size_t budget) noexcept {
    if (bytes <= capacity_bytes_) {
        return {};
    }
    if (!fits_allocation(total, bytes, budget)) {
        return std::unexpected(IoErr::MessageTooLarge);
    }
    void *candidate = dynamic_allocate(bytes);
    if (!candidate) {
        return std::unexpected(IoErr::NoMem);
    }
    const std::size_t charged = dynamic_allocation_bytes(candidate);
    if (!fits(total, charged, budget)) {
        dynamic_deallocate(candidate);
        return std::unexpected(IoErr::MessageTooLarge);
    }
    total += charged;
    total -= storage_bytes();
    dynamic_deallocate(memory_);
    memory_ = candidate;
    capacity_bytes_ = bytes;
    return {};
}
void DynamicSnapshot::sort() noexcept {
    if (size_ < 2) {
        return;
    }
    std::sort(samples_, samples_ + size_, [](const DynamicSample &left, const DynamicSample &right) noexcept {
        for (std::size_t i = 0; i < left.label_values.size(); ++i) {
            const auto a = left.label_values[i];
            const auto b = right.label_values[i];
            const int order = std::memcmp(a.data(), b.data(), std::min(a.size(), b.size()));
            if (order != 0) {
                return order < 0;
            }
            if (a.size() != b.size()) {
                return a.size() < b.size();
            }
        }
        return false;
    });
}

bool DynamicSeriesStore::valid_layout(SeriesOptions options, std::size_t labels, std::size_t label_limit,
                                      std::size_t words) noexcept {
    if (options.mode == SeriesMode::Static) {
        return options.max_series == 0 && options.max_storage_bytes == 0;
    }
    if (options.mode != SeriesMode::Lru || options.max_series == 0) {
        return false;
    }
    // Prove both live storage (+ one unpublished candidate) and a full
    // snapshot have representable upper bounds before accepting the schema.
    std::size_t fixed = sizeof(DynamicFamily) + kAllocationHeader;
    std::size_t entry = sizeof(Slot) + sizeof(std::size_t);
    if (!checked_product(entry, words, sizeof(std::uint64_t)) || !checked_product(fixed, options.max_series, entry)) {
        return false;
    }
    std::size_t label = kAllocationHeader;
    if (!checked_product(label, labels, sizeof(std::size_t)) || !checked_product(label, labels, label_limit) ||
        !checked_product(fixed, options.max_series, label) || !checked_add(fixed, label)) {
        return false;
    }
    std::size_t snapshot = sizeof(DynamicSample);
    return checked_product(snapshot, labels, sizeof(std::string_view)) &&
           checked_product(snapshot, words, sizeof(std::uint64_t)) && checked_product(snapshot, labels, label_limit) &&
           (snapshot == 0 ||
            options.max_series <= (std::numeric_limits<std::size_t>::max() - kAllocationHeader) / snapshot);
}

IoResult<void> DynamicSeriesStore::initialize(const FamilySchema &schema, std::size_t label_limit) noexcept {
    FIBER_ASSERT(!fixed_);
    FIBER_ASSERT(schema.options.mode == SeriesMode::Lru);
    FIBER_ASSERT(schema.type == MetricType::Counter || schema.type == MetricType::Histogram);
    FIBER_ASSERT(valid_layout(schema.options, schema.label_names.size(), label_limit,
                              schema.type == MetricType::Histogram ? schema.upper_bounds.size() + 2 : 1));
    schema_ = &schema;
    label_limit_ = label_limit;
    words_ = schema.type == MetricType::Histogram ? schema.upper_bounds.size() + 2 : 1;
    stats_.capacity = schema.options.max_series;
    const std::size_t bytes = stats_.capacity * (sizeof(Slot) + sizeof(std::size_t) + words_ * sizeof(std::uint64_t));
    stats_.storage_bytes = sizeof(DynamicFamily) + kAllocationHeader;
    if (!fits_allocation(stats_.storage_bytes, bytes, schema.options.max_storage_bytes)) {
        return std::unexpected(IoErr::MessageTooLarge);
    }
    void *candidate = dynamic_allocate(bytes);
    if (!candidate) {
        return std::unexpected(IoErr::NoMem);
    }
    if (!fits(stats_.storage_bytes, dynamic_allocation_bytes(candidate), schema.options.max_storage_bytes)) {
        dynamic_deallocate(candidate);
        return std::unexpected(IoErr::MessageTooLarge);
    }
    fixed_ = candidate;
    stats_.storage_bytes += dynamic_allocation_bytes(fixed_);
    slots_ = static_cast<Slot *>(fixed_);
    index_ = reinterpret_cast<std::size_t *>(slots_ + stats_.capacity);
    values_ = reinterpret_cast<std::uint64_t *>(index_ + stats_.capacity);
    for (std::size_t i = 0; i < stats_.capacity; ++i) {
        std::construct_at(slots_ + i);
        index_[i] = kNone;
    }
    std::fill_n(values_, stats_.capacity * words_, 0);
    return {};
}
DynamicSeriesStore::~DynamicSeriesStore() {
    for (std::size_t i = 0; i < stats_.active_series; ++i) {
        dynamic_deallocate(slots_[i].labels);
    }
    dynamic_deallocate(fixed_);
}
bool DynamicSeriesStore::matches(const Slot &slot, std::span<const std::string_view> labels) const noexcept {
    if (labels.empty()) {
        return true;
    }
    const auto *lengths = static_cast<const std::size_t *>(slot.labels);
    const char *bytes = reinterpret_cast<const char *>(lengths + labels.size());
    for (std::size_t i = 0; i < labels.size(); ++i) {
        if (lengths[i] != labels[i].size() ||
            (lengths[i] != 0 && std::memcmp(bytes, labels[i].data(), lengths[i]) != 0)) {
            return false;
        }
        bytes += lengths[i];
    }
    return true;
}
void DynamicSeriesStore::unlink(std::size_t index) noexcept {
    auto &slot = slots_[index];
    if (slot.previous != kNone) {
        slots_[slot.previous].next = slot.next;
    } else {
        head_ = slot.next;
    }
    if (slot.next != kNone) {
        slots_[slot.next].previous = slot.previous;
    } else {
        tail_ = slot.previous;
    }
}
void DynamicSeriesStore::touch(std::size_t index) noexcept {
    if (index == head_) {
        return;
    }
    auto &slot = slots_[index];
    slot.previous = kNone;
    slot.next = head_;
    if (head_ != kNone) {
        slots_[head_].previous = index;
    } else {
        tail_ = index;
    }
    head_ = index;
}
IoResult<void> DynamicSeriesStore::record(std::span<const std::string_view> labels, std::uint64_t value) noexcept {
    if (labels.size() != schema_->label_names.size()) {
        return std::unexpected(IoErr::Invalid);
    }
    std::size_t label_bytes = labels.size() * sizeof(std::size_t);
    for (std::string_view label: labels) {
        if (label.size() > label_limit_) {
            return std::unexpected(IoErr::Invalid);
        }
        std::size_t escaped = label.size();
        for (char ch: label) {
            if (ch == '\\' || ch == '"' || ch == '\n') {
                if (escaped == label_limit_) {
                    return std::unexpected(IoErr::Invalid);
                }
                ++escaped;
            }
        }
        label_bytes += label.size(); // Upper bound was checked during registration.
    }
    const auto hash = label_hash(labels);
    const auto bucket = static_cast<std::size_t>(hash % stats_.capacity);
    const std::size_t interval =
            schema_->type == MetricType::Histogram
                    ? static_cast<std::size_t>(
                              std::lower_bound(schema_->upper_bounds.begin(), schema_->upper_bounds.end(), value) -
                              schema_->upper_bounds.begin())
                    : 0;
    std::lock_guard guard(mutex_);
    std::size_t index = index_[bucket];
    while (index != kNone && (slots_[index].hash != hash || !matches(slots_[index], labels))) {
        index = slots_[index].hash_next;
    }
    if (index == kNone) {
        const auto fail = [&](IoErr error) -> IoResult<void> {
            ++stats_.creation_failures;
            return std::unexpected(error);
        };
        void *candidate = nullptr;
        if (label_bytes != 0) {
            if (!fits_allocation(stats_.storage_bytes, label_bytes, schema_->options.max_storage_bytes)) {
                return fail(IoErr::MessageTooLarge);
            }
            candidate = dynamic_allocate(label_bytes);
            if (!candidate) {
                return fail(IoErr::NoMem);
            }
            if (!fits(stats_.storage_bytes, dynamic_allocation_bytes(candidate), schema_->options.max_storage_bytes)) {
                dynamic_deallocate(candidate);
                return fail(IoErr::MessageTooLarge);
            }
            auto *lengths = static_cast<std::size_t *>(candidate);
            char *bytes = reinterpret_cast<char *>(lengths + labels.size());
            for (std::size_t i = 0; i < labels.size(); ++i) {
                lengths[i] = labels[i].size();
                if (!labels[i].empty()) {
                    std::memcpy(bytes, labels[i].data(), labels[i].size());
                }
                bytes += labels[i].size();
            }
        }
        // Everything that can fail is complete. Reuse a slot and a hash-chain
        // link; there are no tombstones and no runtime index allocations.
        if (stats_.active_series == stats_.capacity) {
            index = tail_;
            auto &old = slots_[index];
            auto *link = &index_[old.hash % stats_.capacity];
            while (*link != index) {
                link = &slots_[*link].hash_next;
            }
            *link = old.hash_next;
            unlink(index);
            stats_.storage_bytes -= dynamic_allocation_bytes(old.labels);
            dynamic_deallocate(old.labels);
            ++stats_.evictions;
        } else {
            index = stats_.active_series++;
        }
        slots_[index] = Slot{.labels = candidate, .hash = hash, .hash_next = index_[bucket]};
        index_[bucket] = index;
        stats_.storage_bytes += dynamic_allocation_bytes(candidate);
        std::fill_n(values_ + (index * words_), words_, 0);
    } else if (index != head_) {
        unlink(index);
    }
    auto *values = values_ + (index * words_);
    if (schema_->type == MetricType::Counter) {
        values[0] += value;
    } else {
        if (interval < schema_->upper_bounds.size()) {
            ++values[interval];
        }
        ++values[words_ - 2];
        values[words_ - 1] += value;
    }
    touch(index);
    return {};
}
std::size_t DynamicSeriesStore::snapshot_fixed_bytes() const noexcept {
    return stats_.capacity * (sizeof(DynamicSample) + schema_->label_names.size() * sizeof(std::string_view) +
                              words_ * sizeof(std::uint64_t));
}
IoResult<void> DynamicSeriesStore::capture(DynamicSnapshot &snapshot, std::size_t &total, std::size_t budget) noexcept {
    {
        std::lock_guard guard(mutex_);
        const auto labels = schema_->label_names.size();
        std::size_t bytes = snapshot_fixed_bytes();
        for (std::size_t i = 0; i < stats_.active_series; ++i) {
            const auto *lengths = static_cast<const std::size_t *>(slots_[i].labels);
            for (std::size_t j = 0; j < labels; ++j) {
                bytes += lengths[j];
            }
        }
        auto reserved = snapshot.reserve(bytes, total, budget);
        if (!reserved) {
            return reserved;
        }
        snapshot.samples_ = static_cast<DynamicSample *>(snapshot.memory_);
        auto *views = reinterpret_cast<std::string_view *>(snapshot.samples_ + stats_.capacity);
        auto *words = reinterpret_cast<std::uint64_t *>(views + (stats_.capacity * labels));
        char *text = reinterpret_cast<char *>(words + (stats_.capacity * words_));
        for (std::size_t i = 0; i < stats_.active_series; ++i) {
            const auto *lengths = static_cast<const std::size_t *>(slots_[i].labels);
            const char *source = labels == 0 ? nullptr : reinterpret_cast<const char *>(lengths + labels);
            for (std::size_t j = 0; j < labels; ++j) {
                std::memcpy(text, source, lengths[j]);
                views[(i * labels) + j] = std::string_view(text, lengths[j]);
                text += lengths[j];
                source += lengths[j];
            }
            std::memcpy(words + (i * words_), values_ + (i * words_), words_ * sizeof(std::uint64_t));
            snapshot.samples_[i] =
                    DynamicSample{.label_values = {views + (i * labels), labels}, .values = words + (i * words_)};
        }
        snapshot.size_ = stats_.active_series;
    }
    snapshot.sort();
    return {};
}
DynamicSeriesDiagnostics DynamicSeriesStore::diagnostics() const noexcept {
    std::lock_guard guard(mutex_);
    return stats_;
}
} // namespace fiber::prometheus::detail

namespace fiber::prometheus {
fiber::common::IoResult<void> DynamicCounter::inc(std::span<const std::string_view> labels) const noexcept {
    return add(labels, 1);
}
fiber::common::IoResult<void> DynamicCounter::add(std::span<const std::string_view> labels,
                                                  std::uint64_t delta) const noexcept {
    return store_->record(labels, delta);
}
DynamicSeriesDiagnostics DynamicCounter::diagnostics() const noexcept { return store_->diagnostics(); }
fiber::common::IoResult<void> DynamicHistogram::observe(std::span<const std::string_view> labels,
                                                        std::uint64_t value) const noexcept {
    return store_->record(labels, value);
}
DynamicSeriesDiagnostics DynamicHistogram::diagnostics() const noexcept { return store_->diagnostics(); }
} // namespace fiber::prometheus
