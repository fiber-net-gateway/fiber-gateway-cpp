#include "Http2HpackDynamicTable.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

#include "HttpHeaderHash.h"

namespace fiber::http {

namespace {

std::uint64_t hash_header_pair(std::string_view name, std::string_view value) noexcept {
    constexpr std::uint64_t kOffset = 1469598103934665603ULL;
    constexpr std::uint64_t kPrime = 1099511628211ULL;

    std::uint64_t hash = kOffset;
    for (char ch : name) {
        unsigned char lower = static_cast<unsigned char>(ch);
        if (lower >= 'A' && lower <= 'Z') {
            lower = static_cast<unsigned char>(lower - 'A' + 'a');
        }
        hash ^= lower;
        hash *= kPrime;
    }
    hash ^= 0xffU;
    hash *= kPrime;
    for (char ch : value) {
        hash ^= static_cast<unsigned char>(ch);
        hash *= kPrime;
    }
    hash ^= static_cast<std::uint64_t>(name.size());
    hash *= kPrime;
    hash ^= static_cast<std::uint64_t>(value.size());
    hash *= kPrime;
    return hash;
}

} // namespace

bool Http2HpackDynamicTable::init(std::uint32_t storage_cap_bytes) noexcept {
    release();

    storage_cap_bytes_ = storage_cap_bytes;
    bytes_cap_ = storage_cap_bytes;
    max_size_ = storage_cap_bytes;
    entry_cap_ = storage_cap_bytes / kEntryOverhead;

    if (bytes_cap_ != 0) {
        bytes_.reset(new (std::nothrow) char[bytes_cap_]);
        if (!bytes_) {
            release();
            return false;
        }
    }

    if (entry_cap_ == 0) {
        return true;
    }

    entries_.reset(new (std::nothrow) DynamicEntry[entry_cap_]{});
    if (!entries_) {
        release();
        return false;
    }

    if (entry_cap_ > (std::numeric_limits<std::uint32_t>::max() / 2U)) {
        release();
        return false;
    }
    exact_bucket_cap_ = next_pow2(entry_cap_ * 2U);
    exact_bucket_head_.reset(new (std::nothrow) std::uint32_t[exact_bucket_cap_]);
    if (!exact_bucket_head_) {
        release();
        return false;
    }
    std::fill_n(exact_bucket_head_.get(), exact_bucket_cap_, kInvalidSlot);

    return true;
}

void Http2HpackDynamicTable::release() noexcept {
    entries_.reset();
    bytes_.reset();
    exact_bucket_head_.reset();
    entry_cap_ = 0;
    bytes_cap_ = 0;
    exact_bucket_cap_ = 0;
    storage_cap_bytes_ = 0;
    max_size_ = 0;
    current_size_ = 0;
    count_ = 0;
    head_ = 0;
    bytes_begin_ = 0;
    bytes_end_ = 0;
}

void Http2HpackDynamicTable::clear() noexcept {
    current_size_ = 0;
    count_ = 0;
    head_ = 0;
    bytes_begin_ = 0;
    bytes_end_ = 0;
    if (entries_) {
        std::fill_n(entries_.get(), entry_cap_, DynamicEntry{});
    }
    if (exact_bucket_head_) {
        std::fill_n(exact_bucket_head_.get(), exact_bucket_cap_, kInvalidSlot);
    }
}

void Http2HpackDynamicTable::set_max_size(std::uint32_t max_size) noexcept {
    max_size_ = std::min(max_size, storage_cap_bytes_);
    while (current_size_ > max_size_ && count_ != 0) {
        evict_oldest();
    }
    if (count_ == 0) {
        bytes_begin_ = 0;
        bytes_end_ = 0;
    }
}

bool Http2HpackDynamicTable::insert(std::string_view name, std::string_view value) noexcept {
    if (name.size() > std::numeric_limits<std::uint32_t>::max() ||
        value.size() > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }

    const std::uint64_t value_bytes64 = static_cast<std::uint64_t>(name.size()) + static_cast<std::uint64_t>(value.size());
    if (value_bytes64 > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }

    const std::uint32_t value_bytes = static_cast<std::uint32_t>(value_bytes64);
    const std::uint64_t entry_size64 = value_bytes64 + kEntryOverhead;
    if (entry_size64 > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }

    const std::uint32_t entry_size = static_cast<std::uint32_t>(entry_size64);
    if (entry_size > max_size_) {
        clear();
        return false;
    }

    while (current_size_ + entry_size > max_size_ && count_ != 0) {
        evict_oldest();
    }

    if (bytes_cap_ - bytes_end_ < value_bytes) {
        compact_bytes();
    }
    if (bytes_cap_ - bytes_end_ < value_bytes || entry_cap_ == 0 || !entries_ || !bytes_) {
        return false;
    }

    const std::uint32_t slot = count_ == 0 ? 0U : (head_ + entry_cap_ - 1U) % entry_cap_;
    char *dst = bytes_.get() + bytes_end_;
    if (!name.empty()) {
        std::memcpy(dst, name.data(), name.size());
    }
    if (!value.empty()) {
        std::memcpy(dst + name.size(), value.data(), value.size());
    }

    DynamicEntry &entry = entries_[slot];
    entry.data_off = bytes_end_;
    entry.name_len = static_cast<std::uint32_t>(name.size());
    entry.value_len = static_cast<std::uint32_t>(value.size());
    entry.entry_size = entry_size;
    entry.name_hash = http_header_name_hash(name);
    entry.pair_hash = hash_header_pair(name, value);
    entry.exact_prev = kInvalidSlot;
    entry.exact_next = kInvalidSlot;
    entry.live = true;

    bytes_end_ += value_bytes;
    head_ = slot;
    ++count_;
    current_size_ += entry_size;
    link_exact(slot);
    return true;
}

bool Http2HpackDynamicTable::find_exact(std::string_view name, std::string_view value,
                                        std::uint32_t &dynamic_index) const noexcept {
    dynamic_index = 0;
    if (count_ == 0 || exact_bucket_cap_ == 0 || !exact_bucket_head_) {
        return false;
    }

    const std::uint64_t pair_hash = hash_header_pair(name, value);
    std::uint32_t slot = exact_bucket_head_[pair_hash & (exact_bucket_cap_ - 1U)];
    while (slot != kInvalidSlot) {
        const DynamicEntry &entry = entries_[slot];
        if (entry.live && entry.pair_hash == pair_hash && entry.name_len == name.size() &&
            entry.value_len == value.size()) {
            const char *base = bytes_.get() + entry.data_off;
            if (http_header_name_equals_ci(std::string_view(base, entry.name_len), name) &&
                (entry.value_len == 0 ||
                 std::memcmp(base + entry.name_len, value.data(), entry.value_len) == 0)) {
                dynamic_index = slot_to_dynamic_index(slot);
                return true;
            }
        }
        slot = entry.exact_next;
    }
    return false;
}

bool Http2HpackDynamicTable::get_by_index(std::uint32_t dynamic_index, DynamicEntryView &view) const noexcept {
    view = DynamicEntryView{};
    if (dynamic_index == 0 || dynamic_index > count_ || !entries_ || !bytes_) {
        return false;
    }

    const DynamicEntry &entry = entries_[dynamic_index_to_slot(dynamic_index)];
    if (!entry.live) {
        return false;
    }

    const char *base = bytes_.get() + entry.data_off;
    view.name = std::string_view(base, entry.name_len);
    view.value = std::string_view(base + entry.name_len, entry.value_len);
    view.name_hash = entry.name_hash;
    return true;
}

std::uint32_t Http2HpackDynamicTable::next_pow2(std::uint32_t value) noexcept {
    if (value <= 1) {
        return 1;
    }
    value--;
    value |= value >> 1;
    value |= value >> 2;
    value |= value >> 4;
    value |= value >> 8;
    value |= value >> 16;
    return value + 1;
}

std::uint32_t Http2HpackDynamicTable::dynamic_index_to_slot(std::uint32_t dynamic_index) const noexcept {
    return (head_ + dynamic_index - 1U) % entry_cap_;
}

std::uint32_t Http2HpackDynamicTable::slot_to_dynamic_index(std::uint32_t slot) const noexcept {
    return ((slot + entry_cap_ - head_) % entry_cap_) + 1U;
}

std::uint32_t Http2HpackDynamicTable::oldest_slot() const noexcept {
    return dynamic_index_to_slot(count_);
}

void Http2HpackDynamicTable::compact_bytes() noexcept {
    if (count_ == 0) {
        bytes_begin_ = 0;
        bytes_end_ = 0;
        return;
    }
    if (bytes_begin_ == 0) {
        return;
    }

    const std::uint32_t delta = bytes_begin_;
    const std::uint32_t live_bytes = bytes_end_ - bytes_begin_;
    std::memmove(bytes_.get(), bytes_.get() + bytes_begin_, live_bytes);
    for (std::uint32_t i = 1; i <= count_; ++i) {
        DynamicEntry &entry = entries_[dynamic_index_to_slot(i)];
        entry.data_off -= delta;
    }
    bytes_begin_ = 0;
    bytes_end_ = live_bytes;
}

void Http2HpackDynamicTable::evict_oldest() noexcept {
    if (count_ == 0 || !entries_) {
        return;
    }

    const std::uint32_t slot = oldest_slot();
    DynamicEntry &entry = entries_[slot];
    unlink_exact(slot);
    current_size_ -= entry.entry_size;
    bytes_begin_ += entry.name_len + entry.value_len;

    entry = DynamicEntry{};

    --count_;
    if (count_ == 0) {
        current_size_ = 0;
        head_ = 0;
        bytes_begin_ = 0;
        bytes_end_ = 0;
    }
}

void Http2HpackDynamicTable::unlink_exact(std::uint32_t slot) noexcept {
    DynamicEntry &entry = entries_[slot];
    const std::uint32_t bucket = static_cast<std::uint32_t>(entry.pair_hash & (exact_bucket_cap_ - 1U));
    if (entry.exact_prev != kInvalidSlot) {
        entries_[entry.exact_prev].exact_next = entry.exact_next;
    } else {
        exact_bucket_head_[bucket] = entry.exact_next;
    }
    if (entry.exact_next != kInvalidSlot) {
        entries_[entry.exact_next].exact_prev = entry.exact_prev;
    }
    entry.exact_prev = kInvalidSlot;
    entry.exact_next = kInvalidSlot;
}

void Http2HpackDynamicTable::link_exact(std::uint32_t slot) noexcept {
    DynamicEntry &entry = entries_[slot];
    const std::uint32_t bucket = static_cast<std::uint32_t>(entry.pair_hash & (exact_bucket_cap_ - 1U));
    entry.exact_prev = kInvalidSlot;
    entry.exact_next = exact_bucket_head_[bucket];
    if (entry.exact_next != kInvalidSlot) {
        entries_[entry.exact_next].exact_prev = slot;
    }
    exact_bucket_head_[bucket] = slot;
}

} // namespace fiber::http
