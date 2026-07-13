#include "HttpHeaders.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <limits>

#include "HttpHeaderHash.h"

namespace fiber::http {

namespace {

void init_field(HttpHeaders::HeaderField *field, const char *name_ptr, uint32_t name_len, const char *lowcase_ptr,
                const char *value_ptr, uint32_t value_len, uint64_t hash) {
    field->name = name_ptr;
    field->name_len = name_len;
    field->lowcase_name = lowcase_ptr;
    field->value = value_ptr;
    field->value_len = value_len;
    field->name_hash = hash;
    field->next_bucket = nullptr;
    field->next_all = nullptr;
    field->prev_all = nullptr;
}

} // namespace

HttpHeaders::HttpHeaders(mem::BufPool &pool) : pool_(&pool) {}

HttpHeaders::HeaderField *HttpHeaders::add(std::string_view name, std::string_view value) {
    if (!ensure_buckets()) {
        return nullptr;
    }
    HeaderField *field = prepare_owned_field(name, value, nullptr, 0, false);
    if (!field) {
        return nullptr;
    }
    return link_field(field);
}

HttpHeaders::HeaderField *HttpHeaders::add(std::string_view name, std::string_view value, const char *lowcase_name,
                                           uint64_t hash) {
    if (!ensure_buckets()) {
        return nullptr;
    }
    HeaderField *field = prepare_owned_field(name, value, lowcase_name, hash, true);
    if (!field) {
        return nullptr;
    }
    return link_field(field);
}

HttpHeaders::HeaderField *HttpHeaders::add_prehashed(std::string_view name, std::string_view value, uint64_t hash) {
    if (!ensure_buckets()) {
        return nullptr;
    }
    HeaderField *field = prepare_owned_field(name, value, nullptr, hash, true);
    if (!field) {
        return nullptr;
    }
    return link_field(field);
}

HttpHeaders::HeaderField *HttpHeaders::set(std::string_view name, std::string_view value) {
    if (!ensure_buckets()) {
        return nullptr;
    }
    HeaderField *field = prepare_owned_field(name, value, nullptr, 0, false);
    if (!field) {
        return nullptr;
    }
    remove_matching(field->lowcase_view(), field->name_hash, MatchMode::Lowcase);
    return link_field(field);
}

HttpHeaders::HeaderField *HttpHeaders::set(std::string_view name, std::string_view value, const char *lowcase_name,
                                           uint64_t hash) {
    if (!ensure_buckets()) {
        return nullptr;
    }
    HeaderField *field = prepare_owned_field(name, value, lowcase_name, hash, true);
    if (!field) {
        return nullptr;
    }
    remove_matching(field->lowcase_view(), hash, MatchMode::Lowcase);
    return link_field(field);
}

HttpHeaders::HeaderField *HttpHeaders::add_view(std::string_view name, std::string_view value) {
    if (!ensure_buckets()) {
        return nullptr;
    }
    if (name.size() > std::numeric_limits<uint32_t>::max() || value.size() > std::numeric_limits<uint32_t>::max()) {
        return nullptr;
    }
    uint64_t hash = http_header_name_hash(name);
    const char *lowcase_name = name.empty() ? "" : name.data();
    HeaderField *field = prepare_view_field(name, value, lowcase_name, hash);
    if (!field) {
        return nullptr;
    }
    return link_field(field);
}

HttpHeaders::HeaderField *HttpHeaders::add_view(std::string_view name, std::string_view value, const char *lowcase_name,
                                                uint64_t hash) {
    if (!ensure_buckets()) {
        return nullptr;
    }
    HeaderField *field = prepare_view_field(name, value, lowcase_name, hash);
    if (!field) {
        return nullptr;
    }
    return link_field(field);
}

HttpHeaders::HeaderField *HttpHeaders::set_view(std::string_view name, std::string_view value) {
    if (!ensure_buckets()) {
        return nullptr;
    }
    if (name.size() > std::numeric_limits<uint32_t>::max() || value.size() > std::numeric_limits<uint32_t>::max()) {
        return nullptr;
    }
    uint64_t hash = http_header_name_hash(name);
    const char *lowcase_name = name.empty() ? "" : name.data();
    HeaderField *field = prepare_view_field(name, value, lowcase_name, hash);
    if (!field) {
        return nullptr;
    }
    remove_matching(name, hash, MatchMode::CaseInsensitive);
    return link_field(field);
}

HttpHeaders::HeaderField *HttpHeaders::set_view(std::string_view name, std::string_view value, const char *lowcase_name,
                                                uint64_t hash) {
    if (!ensure_buckets()) {
        return nullptr;
    }
    HeaderField *field = prepare_view_field(name, value, lowcase_name, hash);
    if (!field) {
        return nullptr;
    }
    remove_matching(field->lowcase_view(), hash, MatchMode::Lowcase);
    return link_field(field);
}

std::string_view HttpHeaders::get(std::string_view name) const noexcept {
    const HeaderField *node = find_first_node(name);
    if (!node) {
        return {};
    }
    return node->value_view();
}

bool HttpHeaders::contains(std::string_view name) const noexcept { return find_first_node(name) != nullptr; }

std::string_view HttpHeaders::get(std::string_view lowcase_name, uint64_t hash) const noexcept {
    const HeaderField *node = find_first_node_lowcase(lowcase_name, hash);
    if (!node) {
        return {};
    }
    return node->value_view();
}

bool HttpHeaders::contains(std::string_view lowcase_name, uint64_t hash) const noexcept {
    return find_first_node_lowcase(lowcase_name, hash) != nullptr;
}

HttpHeaders::MatchRange HttpHeaders::get_all(std::string_view lowcase_key, uint64_t hash) const noexcept {
    return MatchRange(this, lowcase_key, hash, MatchMode::Lowcase);
}

HttpHeaders::MatchRange HttpHeaders::get_all(std::string_view name) const noexcept {
    return MatchRange(this, name, http_header_name_hash(name), MatchMode::CaseInsensitive);
}

size_t HttpHeaders::remove(std::string_view name) noexcept {
    return remove_matching(name, http_header_name_hash(name), MatchMode::CaseInsensitive);
}

size_t HttpHeaders::remove(std::string_view lowcase_key, uint64_t hash) noexcept {
    return remove_matching(lowcase_key, hash, MatchMode::Lowcase);
}

size_t HttpHeaders::remove_matching(std::string_view key, uint64_t hash, MatchMode mode) noexcept {
    if (!all_head_ || !buckets_) {
        return 0;
    }
    const size_t bucket = hash & (bucket_count_ - 1);

    size_t removed = 0;
    HeaderField *prev_bucket = nullptr;
    HeaderField *node = buckets_[bucket];
    while (node) {
        HeaderField *next = node->next_bucket;
        const bool matches = node->name_hash == hash &&
                             (mode == MatchMode::Lowcase ? node->lowcase_view() == key
                                                         : http_header_name_equals_ci(node->name_view(), key));
        if (matches) {
            if (prev_bucket) {
                prev_bucket->next_bucket = next;
            } else {
                buckets_[bucket] = next;
            }

            if (node->prev_all) {
                node->prev_all->next_all = node->next_all;
            } else {
                all_head_ = node->next_all;
            }
            if (node->next_all) {
                node->next_all->prev_all = node->prev_all;
            } else {
                all_tail_ = node->prev_all;
            }

            ++removed;
            --size_;
        } else {
            prev_bucket = node;
        }
        node = next;
    }
    return removed;
}

bool HttpHeaders::remove(const HeaderField &field) noexcept { return erase(field); }

void HttpHeaders::clear() noexcept {
    all_head_ = nullptr;
    all_tail_ = nullptr;
    size_ = 0;
    if (buckets_) {
        std::fill_n(buckets_, bucket_count_, nullptr);
    }
}

void HttpHeaders::release() noexcept {
    all_head_ = nullptr;
    all_tail_ = nullptr;
    size_ = 0;
    buckets_ = nullptr;
    bucket_count_ = 0;
}

size_t HttpHeaders::size() const noexcept { return size_; }

bool HttpHeaders::erase(const HeaderField &field) noexcept { return unlink_field(const_cast<HeaderField *>(&field)); }

HttpHeaders::ConstIterator HttpHeaders::erase(ConstIterator it) noexcept {
    if (it.node_ == nullptr) {
        return end();
    }
    const HeaderField *next = it.node_->next_all;
    unlink_field(const_cast<HeaderField *>(it.node_));
    return ConstIterator(next);
}

HttpHeaders::MatchIterator HttpHeaders::erase(MatchIterator it) noexcept {
    if (it.headers_ != this || it.node_ == nullptr) {
        return MatchIterator(this, it.key_, it.hash_, it.mode_, nullptr);
    }
    const HeaderField *next = next_match_node(it.node_, it.key_, it.hash_, it.mode_);
    unlink_field(const_cast<HeaderField *>(it.node_));
    return MatchIterator(this, it.key_, it.hash_, it.mode_, next);
}

HttpHeaders::ConstIterator HttpHeaders::begin() const noexcept { return ConstIterator(all_head_); }

HttpHeaders::ConstIterator HttpHeaders::end() const noexcept { return ConstIterator(nullptr); }

bool HttpHeaders::ensure_buckets() noexcept { return buckets_ || rehash_buckets(kDefaultBuckets); }

bool HttpHeaders::rehash_buckets(size_t new_count) noexcept {
    if (new_count < kDefaultBuckets || (new_count & (new_count - 1)) != 0 ||
        new_count > std::numeric_limits<size_t>::max() / sizeof(HeaderField *)) {
        return false;
    }

    auto **new_buckets =
            static_cast<HeaderField **>(pool_->alloc(new_count * sizeof(HeaderField *), alignof(HeaderField *)));
    if (!new_buckets) {
        return false;
    }
    std::fill_n(new_buckets, new_count, nullptr);
    for (HeaderField *node = all_head_; node; node = node->next_all) {
        const size_t bucket = node->name_hash & (new_count - 1);
        node->next_bucket = new_buckets[bucket];
        new_buckets[bucket] = node;
    }
    buckets_ = new_buckets;
    bucket_count_ = new_count;
    return true;
}

const HttpHeaders::HeaderField *HttpHeaders::find_first_node(std::string_view name) const noexcept {
    return find_first_node_ci(name, http_header_name_hash(name));
}

const HttpHeaders::HeaderField *HttpHeaders::find_first_node_ci(std::string_view key, uint64_t hash) const noexcept {
    if (!buckets_) {
        return nullptr;
    }
    const size_t bucket = hash & (bucket_count_ - 1);
    HeaderField *node = buckets_[bucket];
    while (node) {
        if (node->name_hash == hash && http_header_name_equals_ci(node->name_view(), key)) {
            return node;
        }
        node = node->next_bucket;
    }
    return nullptr;
}

const HttpHeaders::HeaderField *HttpHeaders::find_first_node_lowcase(std::string_view lowcase_key,
                                                                     uint64_t hash) const noexcept {
    if (!buckets_) {
        return nullptr;
    }
    const size_t bucket = hash & (bucket_count_ - 1);
    HeaderField *node = buckets_[bucket];
    while (node) {
        if (node->name_hash == hash && node->lowcase_view() == lowcase_key) {
            return node;
        }
        node = node->next_bucket;
    }
    return nullptr;
}

const HttpHeaders::HeaderField *HttpHeaders::next_match_node(const HeaderField *start, std::string_view key,
                                                             uint64_t hash, MatchMode mode) const noexcept {
    return mode == MatchMode::Lowcase ? next_match_node_lowcase(start, key, hash)
                                      : next_match_node_ci(start, key, hash);
}

const HttpHeaders::HeaderField *HttpHeaders::next_match_node_ci(const HeaderField *start, std::string_view key,
                                                                uint64_t hash) const noexcept {
    if (!start) {
        return nullptr;
    }
    const HeaderField *node = start->next_bucket;
    while (node) {
        if (node->name_hash == hash && http_header_name_equals_ci(node->name_view(), key)) {
            return node;
        }
        node = node->next_bucket;
    }
    return nullptr;
}

const HttpHeaders::HeaderField *HttpHeaders::next_match_node_lowcase(const HeaderField *start,
                                                                     std::string_view lowcase_key,
                                                                     uint64_t hash) const noexcept {
    if (!start) {
        return nullptr;
    }
    const HeaderField *node = start->next_bucket;
    while (node) {
        if (node->name_hash == hash && node->lowcase_view() == lowcase_key) {
            return node;
        }
        node = node->next_bucket;
    }
    return nullptr;
}

bool HttpHeaders::unlink_field(HeaderField *field) noexcept {
    if (field == nullptr || !buckets_) {
        return false;
    }

    const size_t bucket = field->name_hash & (bucket_count_ - 1);
    HeaderField *prev_bucket = nullptr;
    HeaderField *node = buckets_[bucket];
    while (node) {
        if (node == field) {
            unlink_field(field, prev_bucket);
            return true;
        }
        prev_bucket = node;
        node = node->next_bucket;
    }
    return false;
}

void HttpHeaders::unlink_field(HeaderField *field, HeaderField *prev_bucket) noexcept {
    if (prev_bucket) {
        prev_bucket->next_bucket = field->next_bucket;
    } else {
        const size_t bucket = field->name_hash & (bucket_count_ - 1);
        buckets_[bucket] = field->next_bucket;
    }

    if (field->prev_all) {
        field->prev_all->next_all = field->next_all;
    } else {
        all_head_ = field->next_all;
    }
    if (field->next_all) {
        field->next_all->prev_all = field->prev_all;
    } else {
        all_tail_ = field->prev_all;
    }

    field->next_bucket = nullptr;
    field->next_all = nullptr;
    field->prev_all = nullptr;
    --size_;
}

const char *HttpHeaders::copy_to_pool(std::string_view data) {
    if (data.empty()) {
        return "";
    }
    char *ptr = static_cast<char *>(pool_->alloc(data.size(), alignof(char)));
    if (!ptr) {
        return nullptr;
    }
    std::memcpy(ptr, data.data(), data.size());
    return ptr;
}

HttpHeaders::HeaderField *HttpHeaders::prepare_owned_field(std::string_view name, std::string_view value,
                                                           const char *lowcase_name, uint64_t hash, bool hash_ready) {
    if (name.size() > std::numeric_limits<uint32_t>::max() || value.size() > std::numeric_limits<uint32_t>::max() ||
        name.size() > std::numeric_limits<size_t>::max() / 2) {
        return nullptr;
    }

    const uint32_t name_len = static_cast<uint32_t>(name.size());
    const char *name_ptr = "";
    const char *lowcase_ptr = "";
    if (name_len > 0) {
        char *names = static_cast<char *>(pool_->alloc(name.size() * 2, alignof(char)));
        if (!names) {
            return nullptr;
        }
        std::memcpy(names, name.data(), name.size());
        char *lowercase = names + name.size();
        if (lowcase_name) {
            std::memcpy(lowercase, lowcase_name, name.size());
        } else if (hash_ready) {
            to_lowercase(name, lowercase);
        } else {
            hash = http_header_name_to_lowercase_and_hash(name, lowercase);
        }
        name_ptr = names;
        lowcase_ptr = lowercase;
    } else if (!hash_ready) {
        hash = 0;
    }

    const char *value_ptr = copy_to_pool(value);
    if (!value_ptr) {
        return nullptr;
    }
    HeaderField *field = alloc_field();
    if (!field) {
        return nullptr;
    }

    init_field(field, name_ptr, name_len, lowcase_ptr, value_ptr, static_cast<uint32_t>(value.size()), hash);
    return field;
}

HttpHeaders::HeaderField *HttpHeaders::prepare_view_field(std::string_view name, std::string_view value,
                                                          const char *lowcase_name, uint64_t hash) {
    if (name.size() > std::numeric_limits<uint32_t>::max() || value.size() > std::numeric_limits<uint32_t>::max() ||
        (!name.empty() && !lowcase_name)) {
        return nullptr;
    }

    HeaderField *field = alloc_field();
    if (!field) {
        return nullptr;
    }

    const uint32_t name_len = static_cast<uint32_t>(name.size());
    const uint32_t value_len = static_cast<uint32_t>(value.size());
    const char *name_ptr = name_len == 0 ? "" : name.data();
    const char *value_ptr = value_len == 0 ? "" : value.data();
    const char *lowcase_ptr = name_len == 0 ? "" : lowcase_name;
    init_field(field, name_ptr, name_len, lowcase_ptr, value_ptr, value_len, hash);
    return field;
}

HttpHeaders::HeaderField *HttpHeaders::link_field(HeaderField *field) noexcept {
    assert(field != nullptr);
    assert(buckets_ != nullptr);
    assert(bucket_count_ >= kDefaultBuckets);
    assert((bucket_count_ & (bucket_count_ - 1)) == 0);

    if (size_ >= bucket_count_ && bucket_count_ <= std::numeric_limits<size_t>::max() / 2) {
        (void) rehash_buckets(bucket_count_ * 2);
    }

    const size_t bucket = field->name_hash & (bucket_count_ - 1);
    field->next_bucket = buckets_[bucket];
    buckets_[bucket] = field;

    if (!all_head_) {
        all_head_ = field;
        all_tail_ = field;
    } else {
        field->prev_all = all_tail_;
        all_tail_->next_all = field;
        all_tail_ = field;
    }
    ++size_;
    return field;
}

} // namespace fiber::http
