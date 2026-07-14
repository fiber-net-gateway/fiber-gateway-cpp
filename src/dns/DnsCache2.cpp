#include "DnsCache2.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

#include "../common/Assert.h"

namespace fiber::dns {

namespace {

constexpr std::size_t kMaxDnsNameLength = kDnsCacheCnameOutSize - 1;

std::size_t align_up(std::size_t value, std::size_t alignment) noexcept {
    return (value + alignment - 1U) & ~(alignment - 1U);
}

bool has_deadline(DnsCache2::TimePoint when) noexcept {
    return when.time_since_epoch() != DnsCache2::TimePoint::duration::zero();
}

bool expired_at(DnsCache2::TimePoint when, DnsCache2::TimePoint now) noexcept {
    return has_deadline(when) && when <= now;
}

} // namespace

struct DnsCache2::AddressSet {
    TimePoint expire_at{};
    std::uint32_t allocation_size = 0;
    std::uint16_t count = 0;
    std::uint16_t cursor = 0;
};

struct DnsCache2::CnameValue {
    TimePoint expire_at{};
    std::uint32_t allocation_size = 0;
    std::uint16_t length = 0;
};

struct DnsCache2::CacheEntry {
    CacheEntry *bucket_next = nullptr;
    CacheEntry *lru_prev = nullptr;
    CacheEntry *lru_next = nullptr;
    AddressSet *a = nullptr;
    AddressSet *aaaa = nullptr;
    CnameValue *cname = nullptr;
    TimePoint negative_expire_at{};
    std::uint64_t hash = 0;
    std::uint32_t allocation_size = 0;
    std::uint16_t name_length = 0;
    EntryKind kind = EntryKind::Address;
    DnsNegativeKind negative = DnsNegativeKind::NxDomain;
};

std::uint64_t dns_cache_hash(std::string_view normalized_name) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned char ch: normalized_name) {
        hash ^= static_cast<std::uint64_t>(ch);
        hash *= 1099511628211ULL;
    }
    hash ^= hash >> 33U;
    hash *= 0xff51afd7ed558ccdULL;
    hash ^= hash >> 33U;
    hash *= 0xc4ceb9fe1a85ec53ULL;
    hash ^= hash >> 33U;
    return hash;
}

DnsCache2::~DnsCache2() { release(); }

bool DnsCache2::init(Options options) noexcept {
    release();
    if (options.max_entries == 0 || options.max_bytes == 0) {
        return false;
    }

    const std::size_t requested_buckets = options.bucket_count == 0 ? options.max_entries : options.bucket_count;
    const std::size_t bucket_count = next_power_of_two(requested_buckets);
    if (bucket_count == 0 || bucket_count > std::numeric_limits<std::size_t>::max() / sizeof(CacheEntry *)) {
        return false;
    }
    const std::size_t bucket_bytes = bucket_count * sizeof(CacheEntry *);
    if (bucket_bytes >= options.max_bytes) {
        return false;
    }

    std::unique_ptr<CacheEntry *[]> buckets(new (std::nothrow) CacheEntry *[bucket_count] {});
    if (!buckets) {
        return false;
    }

    options.bucket_count = bucket_count;
    options_ = options;
    buckets_ = std::move(buckets);
    bucket_count_ = bucket_count;
    bucket_bytes_ = bucket_bytes;
    bytes_used_ = bucket_bytes;
    return true;
}

void DnsCache2::clear() noexcept {
    CacheEntry *entry = lru_head_;
    while (entry != nullptr) {
        CacheEntry *next = entry->lru_next;
        clear_value(*entry);
        FIBER_ASSERT(bytes_used_ >= entry->allocation_size);
        bytes_used_ -= entry->allocation_size;
        free_entry(entry);
        entry = next;
    }
    if (buckets_) {
        std::fill_n(buckets_.get(), bucket_count_, nullptr);
    }
    entry_count_ = 0;
    bytes_used_ = buckets_ ? bucket_bytes_ : 0;
    lru_head_ = nullptr;
    lru_tail_ = nullptr;
}

void DnsCache2::release() noexcept {
    clear();
    buckets_.reset();
    options_ = {};
    bucket_count_ = 0;
    bucket_bytes_ = 0;
    bytes_used_ = 0;
}

bool DnsCache2::valid_key(DnsCacheKey key) noexcept {
    return !key.normalized_name.empty() && key.normalized_name.size() <= kMaxDnsNameLength;
}

std::size_t DnsCache2::next_power_of_two(std::size_t value) noexcept {
    if (value <= 1) {
        return 1;
    }
    std::size_t out = 1;
    while (out < value) {
        if (out > std::numeric_limits<std::size_t>::max() / 2U) {
            return 0;
        }
        out <<= 1U;
    }
    return out;
}

std::string_view DnsCache2::entry_name(const CacheEntry &entry) noexcept {
    const char *data = reinterpret_cast<const char *>(&entry) + sizeof(CacheEntry);
    return std::string_view(data, entry.name_length);
}

net::IpAddress *DnsCache2::address_records(AddressSet &set) noexcept {
    auto *bytes = reinterpret_cast<std::byte *>(&set);
    return reinterpret_cast<net::IpAddress *>(bytes + align_up(sizeof(AddressSet), alignof(net::IpAddress)));
}

const net::IpAddress *DnsCache2::address_records(const AddressSet &set) noexcept {
    const auto *bytes = reinterpret_cast<const std::byte *>(&set);
    return reinterpret_cast<const net::IpAddress *>(bytes + align_up(sizeof(AddressSet), alignof(net::IpAddress)));
}

char *DnsCache2::cname_data(CnameValue &value) noexcept {
    return reinterpret_cast<char *>(&value) + sizeof(CnameValue);
}

const char *DnsCache2::cname_data(const CnameValue &value) noexcept {
    return reinterpret_cast<const char *>(&value) + sizeof(CnameValue);
}

DnsCache2::CacheEntry *DnsCache2::find_entry(DnsCacheKey key) const noexcept {
    if (!buckets_ || bucket_count_ == 0) {
        return nullptr;
    }
    CacheEntry *entry = buckets_[static_cast<std::size_t>(key.hash) & (bucket_count_ - 1U)];
    while (entry != nullptr) {
        if (entry->hash == key.hash && entry->name_length == key.normalized_name.size() &&
            entry_name(*entry) == key.normalized_name) {
            return entry;
        }
        entry = entry->bucket_next;
    }
    return nullptr;
}

DnsCache2::AddressSet *DnsCache2::allocate_address_set(const net::IpAddress *addresses, std::uint16_t count,
                                                       TimePoint expire_at) noexcept {
    const std::size_t records_offset = align_up(sizeof(AddressSet), alignof(net::IpAddress));
    const std::size_t allocation_size = records_offset + static_cast<std::size_t>(count) * sizeof(net::IpAddress);
    if (allocation_size > std::numeric_limits<std::uint32_t>::max()) {
        return nullptr;
    }
    void *storage = ::operator new(allocation_size, std::nothrow);
    if (storage == nullptr) {
        return nullptr;
    }
    auto *set = ::new (storage) AddressSet{};
    set->expire_at = expire_at;
    set->allocation_size = static_cast<std::uint32_t>(allocation_size);
    set->count = count;
    std::memcpy(address_records(*set), addresses, static_cast<std::size_t>(count) * sizeof(net::IpAddress));
    return set;
}

DnsCache2::CnameValue *DnsCache2::allocate_cname(std::string_view target, TimePoint expire_at) noexcept {
    const std::size_t allocation_size = sizeof(CnameValue) + target.size() + 1U;
    if (allocation_size > std::numeric_limits<std::uint32_t>::max()) {
        return nullptr;
    }
    void *storage = ::operator new(allocation_size, std::nothrow);
    if (storage == nullptr) {
        return nullptr;
    }
    auto *value = ::new (storage) CnameValue{};
    value->expire_at = expire_at;
    value->allocation_size = static_cast<std::uint32_t>(allocation_size);
    value->length = static_cast<std::uint16_t>(target.size());
    std::memcpy(cname_data(*value), target.data(), target.size());
    cname_data(*value)[target.size()] = '\0';
    return value;
}

DnsCache2::CacheEntry *DnsCache2::allocate_entry(DnsCacheKey key) noexcept {
    const std::size_t allocation_size = sizeof(CacheEntry) + key.normalized_name.size() + 1U;
    if (allocation_size > std::numeric_limits<std::uint32_t>::max()) {
        return nullptr;
    }
    void *storage = ::operator new(allocation_size, std::nothrow);
    if (storage == nullptr) {
        return nullptr;
    }
    auto *entry = ::new (storage) CacheEntry{};
    entry->hash = key.hash;
    entry->allocation_size = static_cast<std::uint32_t>(allocation_size);
    entry->name_length = static_cast<std::uint16_t>(key.normalized_name.size());
    char *name = reinterpret_cast<char *>(entry) + sizeof(CacheEntry);
    std::memcpy(name, key.normalized_name.data(), key.normalized_name.size());
    name[key.normalized_name.size()] = '\0';
    return entry;
}

void DnsCache2::free_address_set(AddressSet *set) noexcept {
    if (set == nullptr) {
        return;
    }
    set->~AddressSet();
    ::operator delete(set);
}

void DnsCache2::free_cname(CnameValue *value) noexcept {
    if (value == nullptr) {
        return;
    }
    value->~CnameValue();
    ::operator delete(value);
}

void DnsCache2::free_entry(CacheEntry *entry) noexcept {
    if (entry == nullptr) {
        return;
    }
    entry->~CacheEntry();
    ::operator delete(entry);
}

std::size_t DnsCache2::value_bytes(const CacheEntry &entry) const noexcept {
    std::size_t bytes = 0;
    if (entry.a != nullptr) {
        bytes += entry.a->allocation_size;
    }
    if (entry.aaaa != nullptr) {
        bytes += entry.aaaa->allocation_size;
    }
    if (entry.cname != nullptr) {
        bytes += entry.cname->allocation_size;
    }
    return bytes;
}

void DnsCache2::clear_value(CacheEntry &entry) noexcept {
    const std::size_t bytes = value_bytes(entry);
    FIBER_ASSERT(bytes_used_ >= bytes);
    bytes_used_ -= bytes;
    free_address_set(entry.a);
    free_address_set(entry.aaaa);
    free_cname(entry.cname);
    entry.a = nullptr;
    entry.aaaa = nullptr;
    entry.cname = nullptr;
    entry.negative_expire_at = {};
}

void DnsCache2::insert_entry(CacheEntry &entry) noexcept {
    const std::size_t bucket = static_cast<std::size_t>(entry.hash) & (bucket_count_ - 1U);
    entry.bucket_next = buckets_[bucket];
    buckets_[bucket] = &entry;
    ++entry_count_;
    bytes_used_ += entry.allocation_size;
    touch_lru(entry);
}

void DnsCache2::erase_entry(CacheEntry &entry) noexcept {
    const std::size_t bucket = static_cast<std::size_t>(entry.hash) & (bucket_count_ - 1U);
    CacheEntry **link = &buckets_[bucket];
    while (*link != nullptr && *link != &entry) {
        link = &(*link)->bucket_next;
    }
    FIBER_ASSERT(*link == &entry);
    *link = entry.bucket_next;

    unlink_lru(entry);
    clear_value(entry);
    FIBER_ASSERT(bytes_used_ >= entry.allocation_size);
    bytes_used_ -= entry.allocation_size;
    FIBER_ASSERT(entry_count_ != 0);
    --entry_count_;
    free_entry(&entry);
}

void DnsCache2::unlink_lru(CacheEntry &entry) noexcept {
    if (entry.lru_prev != nullptr) {
        entry.lru_prev->lru_next = entry.lru_next;
    } else if (lru_head_ == &entry) {
        lru_head_ = entry.lru_next;
    }
    if (entry.lru_next != nullptr) {
        entry.lru_next->lru_prev = entry.lru_prev;
    } else if (lru_tail_ == &entry) {
        lru_tail_ = entry.lru_prev;
    }
    entry.lru_prev = nullptr;
    entry.lru_next = nullptr;
}

void DnsCache2::touch_lru(CacheEntry &entry) noexcept {
    if (lru_head_ == &entry) {
        return;
    }
    unlink_lru(entry);
    entry.lru_prev = nullptr;
    entry.lru_next = lru_head_;
    if (lru_head_ != nullptr) {
        lru_head_->lru_prev = &entry;
    } else {
        lru_tail_ = &entry;
    }
    lru_head_ = &entry;
}

void DnsCache2::cleanup_expired(CacheEntry &entry, TimePoint now) noexcept {
    if (entry.kind == EntryKind::Address) {
        if (entry.a != nullptr && expired_at(entry.a->expire_at, now)) {
            FIBER_ASSERT(bytes_used_ >= entry.a->allocation_size);
            bytes_used_ -= entry.a->allocation_size;
            free_address_set(entry.a);
            entry.a = nullptr;
        }
        if (entry.aaaa != nullptr && expired_at(entry.aaaa->expire_at, now)) {
            FIBER_ASSERT(bytes_used_ >= entry.aaaa->allocation_size);
            bytes_used_ -= entry.aaaa->allocation_size;
            free_address_set(entry.aaaa);
            entry.aaaa = nullptr;
        }
        return;
    }
    if (entry.kind == EntryKind::Cname && entry.cname != nullptr && expired_at(entry.cname->expire_at, now)) {
        FIBER_ASSERT(bytes_used_ >= entry.cname->allocation_size);
        bytes_used_ -= entry.cname->allocation_size;
        free_cname(entry.cname);
        entry.cname = nullptr;
        return;
    }
    if (entry.kind == EntryKind::Negative && expired_at(entry.negative_expire_at, now)) {
        entry.negative_expire_at = {};
    }
}

bool DnsCache2::ensure_capacity(std::size_t add_bytes, std::size_t replace_bytes, bool add_entry,
                                const CacheEntry *protected_entry) noexcept {
    if (replace_bytes > bytes_used_ || add_bytes > options_.max_bytes - bucket_bytes_) {
        return false;
    }

    std::size_t protected_bytes = 0;
    if (protected_entry != nullptr) {
        const std::size_t protected_value_bytes = value_bytes(*protected_entry);
        if (replace_bytes > protected_value_bytes) {
            return false;
        }
        protected_bytes = protected_entry->allocation_size + protected_value_bytes - replace_bytes;
    }
    if (protected_bytes > options_.max_bytes - bucket_bytes_ - add_bytes) {
        return false;
    }

    while (bytes_used_ - replace_bytes > options_.max_bytes - add_bytes ||
           entry_count_ + (add_entry ? 1U : 0U) > options_.max_entries) {
        CacheEntry *candidate = lru_tail_;
        if (candidate == protected_entry) {
            candidate = candidate->lru_prev;
        }
        if (candidate == nullptr) {
            return false;
        }
        erase_entry(*candidate);
    }
    return true;
}

common::IoErr DnsCache2::lookup(DnsCacheKey key, TimePoint now, DnsCacheOut &out) noexcept {
    out = {};
    if (!buckets_ || !valid_key(key)) {
        return common::IoErr::Invalid;
    }
    CacheEntry *entry = find_entry(key);
    if (entry == nullptr) {
        return common::IoErr::None;
    }

    cleanup_expired(*entry, now);
    const bool empty = (entry->kind == EntryKind::Address && entry->a == nullptr && entry->aaaa == nullptr) ||
                       (entry->kind == EntryKind::Cname && entry->cname == nullptr) ||
                       (entry->kind == EntryKind::Negative && !has_deadline(entry->negative_expire_at));
    if (empty) {
        erase_entry(*entry);
        return common::IoErr::None;
    }

    touch_lru(*entry);
    if (entry->kind == EntryKind::Address) {
        auto append_set = [&out](AddressSet &set) noexcept {
            const net::IpAddress *records = address_records(set);
            for (std::uint16_t i = 0; i < set.count; ++i) {
                const std::uint16_t source = static_cast<std::uint16_t>((set.cursor + i) % set.count);
                out.value.addresses.records[out.value.addresses.count++] = records[source];
            }
            set.cursor = static_cast<std::uint16_t>(set.cursor + 1U == set.count ? 0 : set.cursor + 1U);
        };
        if (entry->a != nullptr) {
            append_set(*entry->a);
        }
        out.value.addresses.v4_count = out.value.addresses.count;
        if (entry->aaaa != nullptr) {
            append_set(*entry->aaaa);
        }
        out.kind = DnsCacheOutKind::Addresses;
        return common::IoErr::None;
    }

    if (entry->kind == EntryKind::Cname) {
        out.value.cname = {};
        std::memcpy(out.value.cname.buf, cname_data(*entry->cname), entry->cname->length);
        out.value.cname.length = entry->cname->length;
        out.value.cname.buf[entry->cname->length] = '\0';
        out.kind = DnsCacheOutKind::Cname;
        return common::IoErr::None;
    }

    out.value.negative = entry->negative;
    out.kind = DnsCacheOutKind::Negative;
    return common::IoErr::None;
}

common::IoErr DnsCache2::upsert_address_set(DnsCacheKey key, const net::IpAddress *addresses, std::uint16_t count,
                                            TimePoint expire_at) noexcept {
    if (!buckets_ || !valid_key(key) || addresses == nullptr || count == 0 || !has_deadline(expire_at)) {
        return common::IoErr::Invalid;
    }
    if (count > kDnsCacheMaxAddressesPerFamily) {
        return common::IoErr::MessageTooLarge;
    }
    const net::IpFamily family = addresses[0].family();
    if (family != net::IpFamily::V4 && family != net::IpFamily::V6) {
        return common::IoErr::Invalid;
    }
    for (std::uint16_t i = 0; i < count; ++i) {
        if (addresses[i].family() != family) {
            return common::IoErr::Invalid;
        }
    }

    AddressSet *new_set = allocate_address_set(addresses, count, expire_at);
    if (new_set == nullptr) {
        return common::IoErr::NoMem;
    }

    CacheEntry *entry = find_entry(key);
    if (entry == nullptr) {
        CacheEntry *new_entry = allocate_entry(key);
        if (new_entry == nullptr) {
            free_address_set(new_set);
            return common::IoErr::NoMem;
        }
        const std::size_t add_bytes = new_entry->allocation_size + new_set->allocation_size;
        if (!ensure_capacity(add_bytes, 0, true, nullptr)) {
            free_entry(new_entry);
            free_address_set(new_set);
            return common::IoErr::NoMem;
        }
        new_entry->kind = EntryKind::Address;
        (family == net::IpFamily::V4 ? new_entry->a : new_entry->aaaa) = new_set;
        insert_entry(*new_entry);
        bytes_used_ += new_set->allocation_size;
        return common::IoErr::None;
    }

    AddressSet *old_set = nullptr;
    std::size_t replace_bytes = 0;
    if (entry->kind == EntryKind::Address) {
        old_set = family == net::IpFamily::V4 ? entry->a : entry->aaaa;
        replace_bytes = old_set == nullptr ? 0 : old_set->allocation_size;
    } else {
        replace_bytes = value_bytes(*entry);
    }
    if (!ensure_capacity(new_set->allocation_size, replace_bytes, false, entry)) {
        free_address_set(new_set);
        return common::IoErr::NoMem;
    }

    if (entry->kind != EntryKind::Address) {
        clear_value(*entry);
        entry->kind = EntryKind::Address;
    } else if (old_set != nullptr) {
        FIBER_ASSERT(bytes_used_ >= old_set->allocation_size);
        bytes_used_ -= old_set->allocation_size;
        free_address_set(old_set);
    }
    (family == net::IpFamily::V4 ? entry->a : entry->aaaa) = new_set;
    bytes_used_ += new_set->allocation_size;
    touch_lru(*entry);
    return common::IoErr::None;
}

common::IoErr DnsCache2::upsert_cname(DnsCacheKey key, std::string_view normalized_target,
                                      TimePoint expire_at) noexcept {
    if (!buckets_ || !valid_key(key) || normalized_target.empty() || normalized_target.size() > kMaxDnsNameLength ||
        normalized_target == key.normalized_name || !has_deadline(expire_at)) {
        return common::IoErr::Invalid;
    }
    CnameValue *new_value = allocate_cname(normalized_target, expire_at);
    if (new_value == nullptr) {
        return common::IoErr::NoMem;
    }

    CacheEntry *entry = find_entry(key);
    if (entry == nullptr) {
        CacheEntry *new_entry = allocate_entry(key);
        if (new_entry == nullptr) {
            free_cname(new_value);
            return common::IoErr::NoMem;
        }
        const std::size_t add_bytes = new_entry->allocation_size + new_value->allocation_size;
        if (!ensure_capacity(add_bytes, 0, true, nullptr)) {
            free_entry(new_entry);
            free_cname(new_value);
            return common::IoErr::NoMem;
        }
        new_entry->kind = EntryKind::Cname;
        new_entry->cname = new_value;
        insert_entry(*new_entry);
        bytes_used_ += new_value->allocation_size;
        return common::IoErr::None;
    }

    const std::size_t replace_bytes = value_bytes(*entry);
    if (!ensure_capacity(new_value->allocation_size, replace_bytes, false, entry)) {
        free_cname(new_value);
        return common::IoErr::NoMem;
    }
    clear_value(*entry);
    entry->kind = EntryKind::Cname;
    entry->cname = new_value;
    bytes_used_ += new_value->allocation_size;
    touch_lru(*entry);
    return common::IoErr::None;
}

common::IoErr DnsCache2::upsert_negative(DnsCacheKey key, DnsNegativeKind negative, TimePoint expire_at) noexcept {
    if (!buckets_ || !valid_key(key) || !has_deadline(expire_at) ||
        (negative != DnsNegativeKind::NxDomain && negative != DnsNegativeKind::NoAddress)) {
        return common::IoErr::Invalid;
    }

    CacheEntry *entry = find_entry(key);
    if (entry == nullptr) {
        CacheEntry *new_entry = allocate_entry(key);
        if (new_entry == nullptr) {
            return common::IoErr::NoMem;
        }
        if (!ensure_capacity(new_entry->allocation_size, 0, true, nullptr)) {
            free_entry(new_entry);
            return common::IoErr::NoMem;
        }
        new_entry->kind = EntryKind::Negative;
        new_entry->negative = negative;
        new_entry->negative_expire_at = expire_at;
        insert_entry(*new_entry);
        return common::IoErr::None;
    }

    clear_value(*entry);
    entry->kind = EntryKind::Negative;
    entry->negative = negative;
    entry->negative_expire_at = expire_at;
    touch_lru(*entry);
    return common::IoErr::None;
}

common::IoErr DnsCache2::erase(DnsCacheKey key) noexcept {
    if (!buckets_ || !valid_key(key)) {
        return common::IoErr::Invalid;
    }
    CacheEntry *entry = find_entry(key);
    if (entry != nullptr) {
        erase_entry(*entry);
    }
    return common::IoErr::None;
}

async::Task<std::size_t> SharedDnsCache2::entry_count() noexcept {
    auto guard = co_await mutex_.lock();
    co_return cache_.entry_count();
}

async::Task<std::size_t> SharedDnsCache2::bytes_used() noexcept {
    auto guard = co_await mutex_.lock();
    co_return cache_.bytes_used();
}

async::Task<common::IoErr> SharedDnsCache2::lookup(DnsCacheKey key, TimePoint now, DnsCacheOut &out) noexcept {
    auto guard = co_await mutex_.lock();
    co_return cache_.lookup(key, now, out);
}

async::Task<common::IoErr> SharedDnsCache2::upsert_address_set(DnsCacheKey key, const net::IpAddress *addresses,
                                                               std::uint16_t count, TimePoint expire_at) noexcept {
    auto guard = co_await mutex_.lock();
    co_return cache_.upsert_address_set(key, addresses, count, expire_at);
}

async::Task<common::IoErr> SharedDnsCache2::upsert_cname(DnsCacheKey key, std::string_view normalized_target,
                                                         TimePoint expire_at) noexcept {
    auto guard = co_await mutex_.lock();
    co_return cache_.upsert_cname(key, normalized_target, expire_at);
}

async::Task<common::IoErr> SharedDnsCache2::upsert_negative(DnsCacheKey key, DnsNegativeKind negative,
                                                            TimePoint expire_at) noexcept {
    auto guard = co_await mutex_.lock();
    co_return cache_.upsert_negative(key, negative, expire_at);
}

async::Task<common::IoErr> SharedDnsCache2::erase(DnsCacheKey key) noexcept {
    auto guard = co_await mutex_.lock();
    co_return cache_.erase(key);
}

} // namespace fiber::dns
