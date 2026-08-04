#ifndef FIBER_HTTP_HEADER_MAP_H
#define FIBER_HTTP_HEADER_MAP_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "HttpHeaderHash.h"

namespace fiber::http {

template<typename V>
class HeaderMap {
    static_assert(std::is_nothrow_move_constructible_v<V>, "HeaderMap values must be nothrow move constructible");
    static_assert(std::is_nothrow_destructible_v<V>, "HeaderMap values must be nothrow destructible");

    struct Entry {
        V value;
        std::uint32_t hash = 0;
        std::uint32_t name_offset = 0;
        std::uint32_t name_size = 0;
    };

    static constexpr std::uint32_t kInvalidIndex = std::numeric_limits<std::uint32_t>::max();
    static constexpr std::size_t kStorageHeaderWords = 1;
    static constexpr std::size_t kMaxNameBytes =
            static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) - (sizeof(std::uint32_t) - 1);
    // Keep bucket_count_for() and the combined storage word count representable on 32-bit size_t.
    static constexpr std::size_t kMaxEntries = std::numeric_limits<std::uint32_t>::max() / 4U;

    static std::uint32_t narrow_hash(std::uint64_t hash) noexcept { return static_cast<std::uint32_t>(hash); }

    static std::uint32_t hash_name(std::string_view name) noexcept { return narrow_hash(http_header_name_hash(name)); }

    static std::size_t next_pow2(std::size_t value) noexcept {
        if (value <= 1) {
            return 1;
        }
        value--;
        value |= value >> 1;
        value |= value >> 2;
        value |= value >> 4;
        value |= value >> 8;
        value |= value >> 16;
        if constexpr (sizeof(std::size_t) >= 8) {
            value |= value >> 32;
        }
        return value + 1;
    }

    static std::size_t bucket_count_for(std::size_t entry_count) noexcept {
        if (entry_count == 0) {
            return 0;
        }
        // Keep the immutable open-addressed table at or below a 75% load factor.
        return next_pow2(entry_count + (entry_count + 2) / 3);
    }

public:
    class EntryView {
    public:
        [[nodiscard]] std::string_view name() const noexcept { return name_; }
        [[nodiscard]] std::uint64_t hash() const noexcept { return hash_; }
        [[nodiscard]] const V &value() const noexcept { return *value_; }

    private:
        friend class HeaderMap;
        friend class ConstIterator;

        EntryView(std::string_view name, std::uint32_t hash, const V &value) noexcept :
            name_(name), hash_(hash), value_(&value) {}

        std::string_view name_;
        std::uint32_t hash_ = 0;
        const V *value_ = nullptr;
    };

    class ConstIterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = EntryView;
        using difference_type = std::ptrdiff_t;
        using pointer = void;
        using reference = EntryView;

        ConstIterator() noexcept = default;

        [[nodiscard]] EntryView operator*() const noexcept { return map_->entry_view(index_); }

        ConstIterator &operator++() noexcept {
            ++index_;
            return *this;
        }

        ConstIterator operator++(int) noexcept {
            ConstIterator copy = *this;
            ++(*this);
            return copy;
        }

        [[nodiscard]] bool operator==(const ConstIterator &other) const noexcept {
            return map_ == other.map_ && index_ == other.index_;
        }

        [[nodiscard]] bool operator!=(const ConstIterator &other) const noexcept { return !(*this == other); }

    private:
        friend class HeaderMap;

        ConstIterator(const HeaderMap *map, std::size_t index) noexcept : map_(map), index_(index) {}

        const HeaderMap *map_ = nullptr;
        std::size_t index_ = 0;
    };

    class Builder {
    public:
        Builder() = default;

        explicit Builder(std::size_t expected_size) {
            if (expected_size > kMaxEntries) {
                expected_size = kMaxEntries;
            }
            entries_.reserve(expected_size);
        }

        Builder(const Builder &) = delete;
        Builder &operator=(const Builder &) = delete;
        Builder(Builder &&) noexcept = default;
        Builder &operator=(Builder &&) noexcept = default;

        // Header names are unique under ASCII case-insensitive comparison. False means
        // either a duplicate name or an input too large for the compact 32-bit layout.
        bool insert(std::string_view name, const V &value) { return insert_impl(name, hash_name(name), value); }
        bool insert(std::string_view name, V &&value) { return insert_impl(name, hash_name(name), std::move(value)); }
        bool insert(std::string_view lowcase_name, std::uint64_t hash, const V &value) {
            return insert_impl(lowcase_name, narrow_hash(hash), value);
        }
        bool insert(std::string_view lowcase_name, std::uint64_t hash, V &&value) {
            return insert_impl(lowcase_name, narrow_hash(hash), std::move(value));
        }

        [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
        [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

        [[nodiscard]] HeaderMap build() && {
            if (entries_.empty()) {
                return HeaderMap{};
            }

            const std::size_t bucket_count = bucket_count_for(entries_.size());
            const std::size_t name_words = (names_.size() + sizeof(std::uint32_t) - 1) / sizeof(std::uint32_t);

            std::vector<std::uint32_t> storage(kStorageHeaderWords + bucket_count + name_words, 0);
            storage[0] = static_cast<std::uint32_t>(bucket_count - 1);
            std::fill_n(storage.begin() + kStorageHeaderWords, bucket_count, kInvalidIndex);

            const std::size_t bucket_mask = bucket_count - 1;
            for (std::size_t index = 0; index < entries_.size(); ++index) {
                std::size_t slot = entries_[index].hash & bucket_mask;
                while (storage[kStorageHeaderWords + slot] != kInvalidIndex) {
                    slot = (slot + 1) & bucket_mask;
                }
                storage[kStorageHeaderWords + slot] = static_cast<std::uint32_t>(index);
            }

            if (!names_.empty()) {
                void *name_storage = storage.data() + kStorageHeaderWords + bucket_count;
                std::memcpy(name_storage, names_.data(), names_.size());
            }

            HeaderMap map;
            map.entries_ = std::move(entries_);
            map.storage_ = std::move(storage);
            return map;
        }

    private:
        [[nodiscard]] std::string_view entry_name(const Entry &entry) const noexcept {
            const char *base = names_.empty() ? "" : names_.data();
            return std::string_view(base + entry.name_offset, entry.name_size);
        }

        template<typename T>
        bool insert_impl(std::string_view name, std::uint32_t hash, T &&value) {
            if (entries_.size() >= kMaxEntries || name.size() > kMaxNameBytes - names_.size()) {
                return false;
            }
            for (const Entry &entry: entries_) {
                if (entry.hash == hash && http_header_name_equals_ci(entry_name(entry), name)) {
                    return false;
                }
            }

            const auto name_offset = static_cast<std::uint32_t>(names_.size());
            Entry entry{
                    .value = std::forward<T>(value),
                    .hash = hash,
                    .name_offset = name_offset,
                    .name_size = static_cast<std::uint32_t>(name.size()),
            };
            names_.insert(names_.end(), name.begin(), name.end());
            entries_.push_back(std::move(entry));
            return true;
        }

        std::vector<Entry> entries_;
        std::vector<char> names_;
    };

    HeaderMap() noexcept = default;
    HeaderMap(const HeaderMap &) = default;
    HeaderMap &operator=(const HeaderMap &) = default;
    HeaderMap(HeaderMap &&) noexcept = default;
    HeaderMap &operator=(HeaderMap &&) noexcept = default;
    ~HeaderMap() = default;

    [[nodiscard]] const V *get(std::string_view name) const noexcept { return get_impl(name, hash_name(name)); }
    [[nodiscard]] const V *get(std::string_view lowcase_name, std::uint64_t hash) const noexcept {
        return get_impl(lowcase_name, narrow_hash(hash));
    }

    [[nodiscard]] bool contains(std::string_view name) const noexcept { return get(name) != nullptr; }
    [[nodiscard]] bool contains(std::string_view lowcase_name, std::uint64_t hash) const noexcept {
        return get(lowcase_name, hash) != nullptr;
    }

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

    [[nodiscard]] ConstIterator begin() const noexcept { return ConstIterator(this, 0); }
    [[nodiscard]] ConstIterator end() const noexcept { return ConstIterator(this, entries_.size()); }

private:
    [[nodiscard]] std::size_t bucket_count() const noexcept {
        return storage_.empty() ? 0 : static_cast<std::size_t>(storage_[0]) + 1;
    }

    [[nodiscard]] const char *name_data() const noexcept {
        if (storage_.empty()) {
            return "";
        }
        return reinterpret_cast<const char *>(storage_.data() + kStorageHeaderWords + bucket_count());
    }

    [[nodiscard]] std::string_view entry_name(const Entry &entry) const noexcept {
        return std::string_view(name_data() + entry.name_offset, entry.name_size);
    }

    [[nodiscard]] EntryView entry_view(std::size_t index) const noexcept {
        const Entry &entry = entries_[index];
        return EntryView(entry_name(entry), entry.hash, entry.value);
    }

    [[nodiscard]] const V *get_impl(std::string_view name, std::uint32_t hash) const noexcept {
        if (entries_.empty()) {
            return nullptr;
        }

        const std::size_t mask = bucket_count() - 1;
        std::size_t slot = hash & mask;
        for (;;) {
            const std::uint32_t index = storage_[kStorageHeaderWords + slot];
            if (index == kInvalidIndex) {
                return nullptr;
            }
            const Entry &entry = entries_[index];
            if (entry.hash == hash && http_header_name_equals_ci(entry_name(entry), name)) {
                return &entry.value;
            }
            slot = (slot + 1) & mask;
        }
    }

    std::vector<Entry> entries_;
    // [bucket mask][open-addressed entry indices][packed header-name bytes].
    std::vector<std::uint32_t> storage_;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HEADER_MAP_H
