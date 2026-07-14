#ifndef FIBER_DNS_DNS_CACHE2_H
#define FIBER_DNS_DNS_CACHE2_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <type_traits>

#include "../async/Mutex.h"
#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../net/IpAddress.h"

namespace fiber::dns {

inline constexpr std::uint16_t kDnsCacheMaxAddressesPerFamily = 16;
inline constexpr std::uint16_t kDnsCacheOutAddressSize = kDnsCacheMaxAddressesPerFamily * 2;
inline constexpr std::uint16_t kDnsCacheCnameOutSize = 256;

struct DnsCacheKey {
    std::string_view normalized_name{};
    std::uint64_t hash = 0;
};

[[nodiscard]] std::uint64_t dns_cache_hash(std::string_view normalized_name) noexcept;

enum class DnsCacheOutKind : std::uint8_t {
    Miss = 0,
    Addresses,
    Cname,
    Negative,
};

enum class DnsNegativeKind : std::uint8_t {
    NxDomain = 0,
    NoAddress,
};

struct DnsCacheAddressOut {
    net::IpAddress records[kDnsCacheOutAddressSize];
    std::uint16_t count;
    std::uint16_t v4_count;
};

struct DnsCacheCnameOut {
    char buf[kDnsCacheCnameOutSize];
    std::uint16_t length;
};

union DnsCacheOutValue {
    DnsCacheAddressOut addresses;
    DnsCacheCnameOut cname;
    DnsNegativeKind negative;
};

struct DnsCacheOut {
    DnsCacheOutKind kind;
    DnsCacheOutValue value;
};

static_assert(std::is_trivially_default_constructible_v<DnsCacheAddressOut>);
static_assert(std::is_trivially_copyable_v<DnsCacheAddressOut>);
static_assert(std::is_trivially_default_constructible_v<DnsCacheOut>);
static_assert(std::is_trivially_copyable_v<DnsCacheOut>);
static_assert(std::is_trivially_destructible_v<DnsCacheOut>);

class DnsCache2 : public common::NonCopyable, public common::NonMovable {
public:
    using TimePoint = std::chrono::steady_clock::time_point;

    struct Options {
        std::size_t max_entries = 1024;
        std::size_t max_bytes = 4 * 1024 * 1024;
        std::size_t bucket_count = 0;
    };

    DnsCache2() noexcept = default;
    ~DnsCache2();

    [[nodiscard]] bool init() noexcept { return init(Options{}); }
    [[nodiscard]] bool init(Options options) noexcept;
    void clear() noexcept;
    void release() noexcept;

    [[nodiscard]] std::size_t entry_count() const noexcept { return entry_count_; }
    [[nodiscard]] std::size_t bytes_used() const noexcept { return bytes_used_; }
    [[nodiscard]] std::size_t bucket_count() const noexcept { return bucket_count_; }

    [[nodiscard]] common::IoErr lookup(DnsCacheKey key, TimePoint now, DnsCacheOut &out) noexcept;
    [[nodiscard]] common::IoErr upsert_address_set(DnsCacheKey key, const net::IpAddress *addresses,
                                                   std::uint16_t count, TimePoint expire_at) noexcept;
    [[nodiscard]] common::IoErr upsert_cname(DnsCacheKey key, std::string_view normalized_target,
                                             TimePoint expire_at) noexcept;
    [[nodiscard]] common::IoErr upsert_negative(DnsCacheKey key, DnsNegativeKind negative,
                                                TimePoint expire_at) noexcept;
    [[nodiscard]] common::IoErr erase(DnsCacheKey key) noexcept;

private:
    enum class EntryKind : std::uint8_t {
        Address,
        Cname,
        Negative,
    };

    struct AddressSet;
    struct CnameValue;
    struct CacheEntry;

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
    void unlink_lru(CacheEntry &entry) noexcept;
    void touch_lru(CacheEntry &entry) noexcept;
    void cleanup_expired(CacheEntry &entry, TimePoint now) noexcept;
    [[nodiscard]] bool ensure_capacity(std::size_t add_bytes, std::size_t replace_bytes, bool add_entry,
                                       const CacheEntry *protected_entry) noexcept;

    Options options_{};
    std::unique_ptr<CacheEntry *[]> buckets_{};
    std::size_t bucket_count_ = 0;
    std::size_t bucket_bytes_ = 0;
    std::size_t entry_count_ = 0;
    std::size_t bytes_used_ = 0;
    CacheEntry *lru_head_ = nullptr;
    CacheEntry *lru_tail_ = nullptr;
};

class SharedDnsCache2 : public common::NonCopyable, public common::NonMovable {
public:
    using Options = DnsCache2::Options;
    using TimePoint = DnsCache2::TimePoint;

    SharedDnsCache2() noexcept = default;

    [[nodiscard]] bool init() noexcept { return cache_.init(); }
    [[nodiscard]] bool init(Options options) noexcept { return cache_.init(options); }
    void release() noexcept { cache_.release(); }

    [[nodiscard]] async::Task<std::size_t> entry_count() noexcept;
    [[nodiscard]] async::Task<std::size_t> bytes_used() noexcept;
    [[nodiscard]] async::Task<common::IoErr> lookup(DnsCacheKey key, TimePoint now, DnsCacheOut &out) noexcept;
    [[nodiscard]] async::Task<common::IoErr> upsert_address_set(DnsCacheKey key, const net::IpAddress *addresses,
                                                                std::uint16_t count, TimePoint expire_at) noexcept;
    [[nodiscard]] async::Task<common::IoErr> upsert_cname(DnsCacheKey key, std::string_view normalized_target,
                                                          TimePoint expire_at) noexcept;
    [[nodiscard]] async::Task<common::IoErr> upsert_negative(DnsCacheKey key, DnsNegativeKind negative,
                                                             TimePoint expire_at) noexcept;
    [[nodiscard]] async::Task<common::IoErr> erase(DnsCacheKey key) noexcept;

private:
    DnsCache2 cache_{};
    async::Mutex mutex_{};
};

} // namespace fiber::dns

#endif // FIBER_DNS_DNS_CACHE2_H
