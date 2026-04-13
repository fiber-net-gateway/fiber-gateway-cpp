#ifndef FIBER_HTTP_HEADER_MAP_H
#define FIBER_HTTP_HEADER_MAP_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "HttpHeaderHash.h"

namespace fiber::http {

template<typename V>
class HeaderMap {
public:
    HeaderMap() { init_buckets(kDefaultBuckets); }
    explicit HeaderMap(size_t bucket_count) { init_buckets(bucket_count); }

    bool insert(std::string_view name, const V &value) { return insert_impl(name, hash_name(name), value); }
    bool insert(std::string_view name, V &&value) { return insert_impl(name, hash_name(name), std::move(value)); }
    bool insert(std::string_view lowcase_name, std::uint64_t hash, const V &value) {
        return insert_impl(lowcase_name, narrow_hash(hash), value);
    }
    bool insert(std::string_view lowcase_name, std::uint64_t hash, V &&value) {
        return insert_impl(lowcase_name, narrow_hash(hash), std::move(value));
    }

    V *get(std::string_view name) { return get_impl(name, hash_name(name)); }
    const V *get(std::string_view name) const { return get_impl(name, hash_name(name)); }
    V *get(std::string_view name, std::uint64_t hash) { return get_impl(name, narrow_hash(hash)); }
    const V *get(std::string_view name, std::uint64_t hash) const { return get_impl(name, narrow_hash(hash)); }

    size_t size() const noexcept { return nodes_.size(); }

private:
    struct Node {
        std::string key;
        std::uint32_t hash = 0;
        std::uint32_t next = kInvalidIndex;
        V value;
    };

    static constexpr std::uint32_t kInvalidIndex = 0xFFFFFFFFu;
    static constexpr size_t kDefaultBuckets = 32;

    static std::uint32_t narrow_hash(std::uint64_t hash) noexcept { return static_cast<std::uint32_t>(hash); }

    static std::uint32_t hash_name(std::string_view name) noexcept { return narrow_hash(http_header_name_hash(name)); }

    static size_t next_pow2(size_t value) {
        if (value <= 1) {
            return 1;
        }
        value--;
        value |= value >> 1;
        value |= value >> 2;
        value |= value >> 4;
        value |= value >> 8;
        value |= value >> 16;
        if constexpr (sizeof(size_t) >= 8) {
            value |= value >> 32;
        }
        return value + 1;
    }

    void init_buckets(size_t count) {
        if (count == 0) {
            count = kDefaultBuckets;
        }
        count = next_pow2(count);
        if (count < kDefaultBuckets) {
            count = kDefaultBuckets;
        }
        bucket_head_.assign(count, kInvalidIndex);
    }

    template<typename T>
    bool insert_impl(std::string_view key, std::uint32_t hash, T &&value) {
        if (bucket_head_.empty()) {
            init_buckets(kDefaultBuckets);
        }
        std::uint32_t bucket = hash & static_cast<std::uint32_t>(bucket_head_.size() - 1);
        std::uint32_t index = bucket_head_[bucket];
        while (index != kInvalidIndex) {
            const auto &node = nodes_[index];
            if (node.hash == hash && http_header_name_equals_ci(node.key, key)) {
                return false;
            }
            index = node.next;
        }

        Node node{std::string(key), hash, bucket_head_[bucket], std::forward<T>(value)};
        nodes_.push_back(std::move(node));
        bucket_head_[bucket] = static_cast<std::uint32_t>(nodes_.size() - 1);
        return true;
    }

    V *get_impl(std::string_view key, std::uint32_t hash) {
        return const_cast<V *>(static_cast<const HeaderMap *>(this)->get_impl(key, hash));
    }

    const V *get_impl(std::string_view key, std::uint32_t hash) const {
        if (bucket_head_.empty()) {
            return nullptr;
        }
        std::uint32_t bucket = hash & static_cast<std::uint32_t>(bucket_head_.size() - 1);
        std::uint32_t index = bucket_head_[bucket];
        while (index != kInvalidIndex) {
            const auto &node = nodes_[index];
            if (node.hash == hash && http_header_name_equals_ci(node.key, key)) {
                return &node.value;
            }
            index = node.next;
        }
        return nullptr;
    }

    std::vector<Node> nodes_;
    std::vector<std::uint32_t> bucket_head_;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HEADER_MAP_H
