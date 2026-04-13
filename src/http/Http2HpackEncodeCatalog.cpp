#include "Http2HpackEncodeCatalog.h"

#include <cstring>
#include <new>

#include "HttpHeaderHash.h"

namespace fiber::http {

namespace {

constexpr std::uint32_t kEntryOverhead = 32;

inline bool same_bytes(std::string_view left, std::string_view right) noexcept {
    return left.size() == right.size() && std::memcmp(left.data(), right.data(), right.size()) == 0;
}

} // namespace

bool Http2HpackEncodeCatalog::init(std::span<const PolicyEntry> policy_entries) noexcept {
    release();

    const std::uint32_t static_count = Http2HpackStaticTable::kEntryCount;
    const std::uint32_t policy_count = static_cast<std::uint32_t>(policy_entries.size());
    const std::uint32_t entry_count = static_count + policy_count;
    if (entry_count == 0) {
        return true;
    }

    std::uint32_t bucket_cap = next_pow2(entry_count * 2);
    std::unique_ptr<EntryView[]> entries(new (std::nothrow) EntryView[entry_count]);
    std::unique_ptr<std::uint32_t[]> bucket_head(new (std::nothrow) std::uint32_t[bucket_cap]);
    std::unique_ptr<std::uint32_t[]> next_bucket(new (std::nothrow) std::uint32_t[entry_count]);
    if (!entries || !bucket_head || !next_bucket) {
        return false;
    }

    for (std::uint32_t i = 0; i < bucket_cap; ++i) {
        bucket_head[i] = kInvalidId;
    }

    for (std::uint32_t i = 0; i < static_count; ++i) {
        Http2HpackStaticTable::TableEntryView static_view;
        if (!Http2HpackStaticTable::get_by_index(i + 1, static_view)) {
            return false;
        }

        EntryView &entry = entries[i];
        entry.kind = EntryKind::Static;
        entry.catalog_id = i;
        entry.policy_slot = kInvalidId;
        entry.name = static_view.name;
        entry.name_hash = static_view.name_hash;
        entry.value = static_view.value;
        entry.hpack_index = i + 1;
        entry.entry_size = static_cast<std::uint32_t>(kEntryOverhead + entry.name.size() + entry.value.size());
    }

    for (std::uint32_t i = 0; i < policy_count; ++i) {
        const PolicyEntry &policy = policy_entries[i];
        EntryView &entry = entries[static_count + i];
        entry.kind = EntryKind::Policy;
        entry.catalog_id = static_count + i;
        entry.policy_slot = i;
        entry.name = policy.name;
        entry.name_hash = policy.name_hash != 0 ? policy.name_hash : http_header_name_hash(policy.name);
        entry.value = policy.value;
        entry.hpack_index = 0;
        entry.entry_size = static_cast<std::uint32_t>(kEntryOverhead + entry.name.size() + entry.value.size());
    }

    for (std::uint32_t i = 0; i < entry_count; ++i) {
        EntryView &entry = entries[i];
        std::uint32_t bucket = entry.name_hash & (bucket_cap - 1);
        next_bucket[i] = bucket_head[bucket];
        bucket_head[bucket] = i;
    }

    entries_ = std::move(entries);
    bucket_head_ = std::move(bucket_head);
    next_bucket_ = std::move(next_bucket);
    entry_count_ = entry_count;
    policy_count_ = policy_count;
    bucket_cap_ = bucket_cap;
    return true;
}

void Http2HpackEncodeCatalog::release() noexcept {
    entries_.reset();
    bucket_head_.reset();
    next_bucket_.reset();
    entry_count_ = 0;
    policy_count_ = 0;
    bucket_cap_ = 0;
}

bool Http2HpackEncodeCatalog::is_static_entry(const EntryView *entry) const noexcept {
    return entry != nullptr && entry->kind == EntryKind::Static;
}

bool Http2HpackEncodeCatalog::is_policy_entry(const EntryView *entry) const noexcept {
    return entry != nullptr && entry->kind == EntryKind::Policy;
}

Http2HpackEncodeCatalog::FindResult Http2HpackEncodeCatalog::find(std::string_view name, std::uint64_t name_hash,
                                                                  std::string_view value) const noexcept {
    if (!entries_ || !bucket_head_ || !next_bucket_ || bucket_cap_ == 0) {
        return {};
    }

    FindResult name_result;
    FindResult exact_result;
    const EntryView *name_entry = nullptr;
    const EntryView *exact_entry = nullptr;
    const std::uint32_t bucket = name_hash & (bucket_cap_ - 1);
    for (std::uint32_t id = bucket_head_[bucket]; id != kInvalidId; id = next_bucket_[id]) {
        const EntryView &entry = entries_[id];
        if (entry.name_hash != name_hash || entry.name.size() != name.size()) {
            continue;
        }
        if (!http_header_name_equals_ci(name, entry.name)) {
            continue;
        }

        if (!name_entry || prefer_entry(entry, *name_entry)) {
            name_result.entry = &entry;
            name_entry = &entry;
        }

        if (same_bytes(value, entry.value) && (!exact_entry || prefer_entry(entry, *exact_entry))) {
            exact_result.entry = &entry;
            exact_result.exact = true;
            exact_entry = &entry;
        }
    }

    if (exact_result.entry != nullptr) {
        return exact_result;
    }
    return name_result;
}

std::uint32_t Http2HpackEncodeCatalog::next_pow2(std::uint32_t value) noexcept {
    if (value <= 1) {
        return 1;
    }
    --value;
    value |= value >> 1;
    value |= value >> 2;
    value |= value >> 4;
    value |= value >> 8;
    value |= value >> 16;
    return value + 1;
}

bool Http2HpackEncodeCatalog::prefer_entry(const EntryView &candidate, const EntryView &current) noexcept {
    if (candidate.kind != current.kind) {
        return candidate.kind == EntryKind::Static;
    }
    return candidate.hpack_index != 0 && (current.hpack_index == 0 || candidate.hpack_index < current.hpack_index);
}

} // namespace fiber::http
