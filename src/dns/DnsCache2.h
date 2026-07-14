#ifndef FIBER_DNS_DNS_CACHE2_H
#define FIBER_DNS_DNS_CACHE2_H

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <type_traits>

#include "../common/BinaryHeap.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../event/EventLoop.h"
#include "DnsAddress.h"

namespace fiber::dns {

inline constexpr std::uint16_t kDnsCacheCnameOutSize = 256;
inline constexpr std::uint8_t kDnsCacheV4FamilyMask = 1U << 0U;
inline constexpr std::uint8_t kDnsCacheV6FamilyMask = 1U << 1U;

struct DnsCacheKey {
    std::string_view normalized_name{};
    std::uint64_t hash = 0;
};

[[nodiscard]] std::uint64_t dns_cache_hash(std::string_view normalized_name) noexcept;

enum class DnsCacheOutKind : std::uint8_t {
    Miss = 0,
    Addresses,
    Cname,
    NxDomain,
};

struct DnsCacheAddressOut {
    DnsAddressSet address_set;
    // An expiry is meaningful only when the corresponding family is cached.
    std::chrono::steady_clock::time_point v4_expire_at;
    std::chrono::steady_clock::time_point v6_expire_at;
    std::uint8_t cached_family_mask;

    [[nodiscard]] bool has_v4() const noexcept { return (cached_family_mask & kDnsCacheV4FamilyMask) != 0; }
    [[nodiscard]] bool has_v6() const noexcept { return (cached_family_mask & kDnsCacheV6FamilyMask) != 0; }
};

struct DnsCacheCnameOut {
    char buf[kDnsCacheCnameOutSize];
    std::uint16_t length;
};

union DnsCacheOutValue {
    DnsCacheAddressOut addresses;
    DnsCacheCnameOut cname;
};

struct DnsCacheOut {
    DnsCacheOut() noexcept : kind(DnsCacheOutKind::Miss), value{} {}

    DnsCacheOutKind kind;
    DnsCacheOutValue value;
};

static_assert(std::is_trivially_copyable_v<DnsCacheAddressOut>);
static_assert(std::is_trivially_destructible_v<DnsCacheAddressOut>);
static_assert(std::is_nothrow_default_constructible_v<DnsCacheOut>);
static_assert(std::is_trivially_copyable_v<DnsCacheOut>);
static_assert(std::is_trivially_destructible_v<DnsCacheOut>);

class DnsCache2 : public common::NonCopyable, public common::NonMovable {
public:
    using TimePoint = std::chrono::steady_clock::time_point;

    struct Options {
        std::size_t max_entries = 1024;
        std::size_t max_bytes = 4 * 1024 * 1024;
        // Zero selects a fixed bucket array sized for a maximum load factor of 0.5.
        // A nonzero value is a fixed override, rounded up to a power of two.
        std::size_t bucket_count = 0;
    };

    DnsCache2() noexcept;
    ~DnsCache2();

    [[nodiscard]] bool init() noexcept { return init(Options{}); }
    [[nodiscard]] bool init(Options options) noexcept;
    void clear() noexcept;
    void release() noexcept;

    [[nodiscard]] std::size_t entry_count() const noexcept { return entry_count_; }
    [[nodiscard]] std::size_t bytes_used() const noexcept { return bytes_used_; }
    [[nodiscard]] std::size_t bucket_count() const noexcept { return bucket_count_; }

    [[nodiscard]] common::IoErr lookup(DnsCacheKey key, TimePoint now, DnsCacheOut &out) noexcept;
    // A zero count stores NoData for the selected family; addresses may then be null.
    [[nodiscard]] common::IoErr upsert_address_set(DnsCacheKey key, net::IpFamily family,
                                                   const net::IpAddress *addresses, std::uint16_t count,
                                                   TimePoint expire_at) noexcept;
    [[nodiscard]] common::IoErr upsert_cname(DnsCacheKey key, std::string_view normalized_target,
                                             TimePoint expire_at) noexcept;
    [[nodiscard]] common::IoErr upsert_nxdomain(DnsCacheKey key, TimePoint expire_at) noexcept;
    [[nodiscard]] common::IoErr erase(DnsCacheKey key) noexcept;

private:
    friend class SharedDnsCache2;

    enum class EntryKind : std::uint8_t {
        Address,
        Cname,
        NxDomain,
    };

    struct AddressSet;
    struct CnameValue;
    struct CacheEntry {
        CacheEntry *bucket_next = nullptr;
        common::BinaryHeapNode expiry_node{};
        AddressSet *a = nullptr;
        AddressSet *aaaa = nullptr;
        CnameValue *cname = nullptr;
        TimePoint next_expire_at{};
        TimePoint nxdomain_expire_at{};
        std::uint64_t hash = 0;
        std::uint32_t allocation_size = 0;
        std::uint16_t name_length = 0;
        EntryKind kind = EntryKind::Address;
        bool in_expiry_heap = false;
    };

    struct ExpiryCompare {
        bool operator()(const CacheEntry *left, const CacheEntry *right) const noexcept;
    };

    using ExpiryHeap = common::BinaryHeap<CacheEntry, offsetof(CacheEntry, expiry_node), ExpiryCompare>;

    [[nodiscard]] static bool valid_key(DnsCacheKey key) noexcept;
    [[nodiscard]] static std::size_t next_power_of_two(std::size_t value) noexcept;
    [[nodiscard]] static std::string_view entry_name(const CacheEntry &entry) noexcept;
    [[nodiscard]] static net::IpAddress *address_records(AddressSet &set) noexcept;
    [[nodiscard]] static const net::IpAddress *address_records(const AddressSet &set) noexcept;
    [[nodiscard]] static char *cname_data(CnameValue &value) noexcept;
    [[nodiscard]] static const char *cname_data(const CnameValue &value) noexcept;

    [[nodiscard]] CacheEntry *find_entry(DnsCacheKey key) const noexcept;
    [[nodiscard]] AddressSet *allocate_address_set(const net::IpAddress *addresses, std::uint16_t count,
                                                   TimePoint expire_at) noexcept;
    [[nodiscard]] CnameValue *allocate_cname(std::string_view target, TimePoint expire_at) noexcept;
    [[nodiscard]] CacheEntry *allocate_entry(DnsCacheKey key) noexcept;
    static void free_address_set(AddressSet *set) noexcept;
    static void free_cname(CnameValue *value) noexcept;
    static void free_entry(CacheEntry *entry) noexcept;

    [[nodiscard]] std::size_t value_bytes(const CacheEntry &entry) const noexcept;
    void clear_value(CacheEntry &entry) noexcept;
    void insert_entry(CacheEntry &entry) noexcept;
    void erase_entry(CacheEntry &entry) noexcept;
    [[nodiscard]] TimePoint entry_expire_at(const CacheEntry &entry) const noexcept;
    void unlink_expiry(CacheEntry &entry) noexcept;
    void refresh_expiry(CacheEntry &entry) noexcept;
    void cleanup_expired(CacheEntry &entry, TimePoint now) noexcept;
    void expire_due(TimePoint now) noexcept;
    [[nodiscard]] TimePoint next_expire_at() const noexcept;
    [[nodiscard]] bool ensure_capacity(std::size_t add_bytes, std::size_t replace_bytes, bool add_entry,
                                       const CacheEntry *protected_entry) noexcept;

    Options options_{};
    std::unique_ptr<CacheEntry *[]> buckets_{};
    ExpiryHeap expiry_heap_{};
    std::size_t bucket_count_ = 0;
    std::size_t bucket_bytes_ = 0;
    std::size_t entry_count_ = 0;
    std::size_t bytes_used_ = 0;
};

class SharedDnsCache2 : public common::NonCopyable, public common::NonMovable {
public:
    using Options = DnsCache2::Options;
    using TimePoint = DnsCache2::TimePoint;

    SharedDnsCache2() noexcept = default;
    ~SharedDnsCache2();

    [[nodiscard]] bool init(event::EventLoop &owner_loop) noexcept { return init(owner_loop, Options{}); }
    [[nodiscard]] bool init(event::EventLoop &owner_loop, Options options) noexcept;

    // Must be posted to the bound EventLoop after callers have stopped issuing cache operations.
    void shutdown() noexcept;

    [[nodiscard]] std::size_t entry_count() noexcept;
    [[nodiscard]] std::size_t bytes_used() noexcept;
    [[nodiscard]] common::IoErr lookup(DnsCacheKey key, TimePoint now, DnsCacheOut &out) noexcept;
    [[nodiscard]] common::IoErr upsert_address_set(DnsCacheKey key, net::IpFamily family,
                                                   const net::IpAddress *addresses, std::uint16_t count,
                                                   TimePoint expire_at) noexcept;
    [[nodiscard]] common::IoErr upsert_cname(DnsCacheKey key, std::string_view normalized_target,
                                             TimePoint expire_at) noexcept;
    [[nodiscard]] common::IoErr upsert_nxdomain(DnsCacheKey key, TimePoint expire_at) noexcept;
    [[nodiscard]] common::IoErr erase(DnsCacheKey key) noexcept;

private:
    static constexpr std::chrono::milliseconds kMaintenanceRetryDelay{1};

    void request_timer_rearm() noexcept;
    void maintain_and_rearm() noexcept;
    void arm_timer(TimePoint deadline) noexcept;
    static void on_rearm(SharedDnsCache2 *cache) noexcept;
    static void on_expiry_timer(SharedDnsCache2 *cache) noexcept;

    DnsCache2 cache_{};
    std::mutex mutex_{};
    event::EventLoop *owner_loop_ = nullptr;
    event::EventLoop::NotifyEntry rearm_entry_{};
    event::EventLoop::TimerEntry expiry_timer_{};
    std::atomic<bool> rearm_pending_{false};
    std::atomic<bool> stopping_{true};
};

} // namespace fiber::dns

#endif // FIBER_DNS_DNS_CACHE2_H
