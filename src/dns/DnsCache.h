#ifndef FIBER_DNS_DNS_CACHE_H
#define FIBER_DNS_DNS_CACHE_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../net/IpAddress.h"
#include "DnsProtocol.h"

namespace fiber::dns {

class NameSnapshot : public common::NonCopyable, public common::NonMovable {
public:
    struct AddressView {
        const net::IpAddress *records = nullptr;
        std::uint16_t count = 0;
        bool present = false;
        bool negative = false;
        std::chrono::steady_clock::time_point expire_at{};
    };

    struct CnameView {
        std::string_view target{};
        bool present = false;
        std::chrono::steady_clock::time_point expire_at{};
    };

    struct Options {
        std::uint16_t max_a_records = 8;
        std::uint16_t max_aaaa_records = 8;
        std::uint16_t max_name_storage = 512;
    };

    NameSnapshot() noexcept = default;

    [[nodiscard]] bool init() noexcept { return init(Options{}); }
    [[nodiscard]] bool init(Options options) noexcept;
    void release() noexcept;
    void clear() noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool found() const noexcept { return found_; }
    [[nodiscard]] bool has_nxdomain() const noexcept { return has_nxdomain_; }
    [[nodiscard]] std::chrono::steady_clock::time_point nxdomain_expire_at() const noexcept {
        return nxdomain_expire_at_;
    }
    [[nodiscard]] const AddressView &a() const noexcept { return a_; }
    [[nodiscard]] const AddressView &aaaa() const noexcept { return aaaa_; }
    [[nodiscard]] const CnameView &cname() const noexcept { return cname_; }

private:
    friend class DnsCache;

    [[nodiscard]] common::IoErr assign_a(const net::IpAddress *records,
                                         std::uint16_t count,
                                         bool negative,
                                         std::chrono::steady_clock::time_point expire_at) noexcept;
    [[nodiscard]] common::IoErr assign_aaaa(const net::IpAddress *records,
                                            std::uint16_t count,
                                            bool negative,
                                            std::chrono::steady_clock::time_point expire_at) noexcept;
    [[nodiscard]] common::IoErr assign_cname(std::string_view target,
                                             std::chrono::steady_clock::time_point expire_at) noexcept;
    void assign_nxdomain(std::chrono::steady_clock::time_point expire_at) noexcept;

    Options options_{};
    std::unique_ptr<net::IpAddress[]> a_storage_{};
    std::unique_ptr<net::IpAddress[]> aaaa_storage_{};
    std::unique_ptr<char[]> name_storage_{};
    bool found_ = false;
    bool has_nxdomain_ = false;
    std::chrono::steady_clock::time_point nxdomain_expire_at_{};
    AddressView a_{};
    AddressView aaaa_{};
    CnameView cname_{};
};

class DnsCache : public common::NonCopyable, public common::NonMovable {
public:
    struct Options {
        std::size_t max_entries = 1024;
        std::size_t max_bytes = 4 * 1024 * 1024;
        std::size_t index_capacity = 0;
        std::uint16_t eviction_sample = 16;
    };

    DnsCache() noexcept = default;

    [[nodiscard]] bool init() noexcept { return init(Options{}); }
    [[nodiscard]] bool init(Options options) noexcept;
    void release() noexcept;

    [[nodiscard]] std::size_t entry_count() const noexcept;
    [[nodiscard]] std::size_t bytes_used() const noexcept;

    [[nodiscard]] common::IoErr lookup_name(std::string_view qname,
                                            std::uint16_t qclass,
                                            std::chrono::steady_clock::time_point now,
                                            NameSnapshot &out) noexcept;

    [[nodiscard]] common::IoErr upsert_a(std::string_view qname,
                                         std::uint16_t qclass,
                                         const net::IpAddress *records,
                                         std::uint16_t count,
                                         std::chrono::steady_clock::time_point expire_at) noexcept;
    [[nodiscard]] common::IoErr upsert_aaaa(std::string_view qname,
                                            std::uint16_t qclass,
                                            const net::IpAddress *records,
                                            std::uint16_t count,
                                            std::chrono::steady_clock::time_point expire_at) noexcept;
    [[nodiscard]] common::IoErr upsert_cname(std::string_view qname,
                                             std::uint16_t qclass,
                                             std::string_view target,
                                             std::chrono::steady_clock::time_point expire_at) noexcept;
    [[nodiscard]] common::IoErr upsert_negative_nxdomain(
        std::string_view qname,
        std::uint16_t qclass,
        std::chrono::steady_clock::time_point expire_at) noexcept;
    [[nodiscard]] common::IoErr upsert_negative_nodata(std::string_view qname,
                                                       std::uint16_t qclass,
                                                       std::uint16_t qtype,
                                                       std::chrono::steady_clock::time_point expire_at) noexcept;
    [[nodiscard]] common::IoErr erase(std::string_view qname, std::uint16_t qclass) noexcept;
    [[nodiscard]] std::size_t sweep_expired(std::chrono::steady_clock::time_point now,
                                            std::size_t budget) noexcept;

private:
    enum class SlotState : std::uint8_t {
        Empty,
        Positive,
        NegativeNoData,
    };

    struct AddressSlot {
        SlotState state = SlotState::Empty;
        std::uint16_t count = 0;
        std::uint32_t blob_offset = 0;
        std::chrono::steady_clock::time_point expire_at{};
    };

    struct CnameSlot {
        bool present = false;
        std::uint16_t target_len = 0;
        std::uint32_t blob_offset = 0;
        std::chrono::steady_clock::time_point expire_at{};
    };

    struct NameEntry {
        std::uint64_t hash = 0;
        std::uint16_t qclass = 0;
        std::uint16_t owner_len = 0;
        std::uint32_t blob_size = 0;
        char *blob = nullptr;
        AddressSlot a{};
        AddressSlot aaaa{};
        CnameSlot cname{};
        std::chrono::steady_clock::time_point nxdomain_expire_at{};
        std::uint64_t approx_last_access = 0;
        std::uint32_t next_free = 0;
        bool occupied = false;
    };

    struct AddressState {
        SlotState state = SlotState::Empty;
        const net::IpAddress *records = nullptr;
        std::uint16_t count = 0;
        std::chrono::steady_clock::time_point expire_at{};
    };

    struct CnameState {
        bool present = false;
        std::string_view target{};
        std::chrono::steady_clock::time_point expire_at{};
    };

    struct EntryState {
        std::string_view owner{};
        AddressState a{};
        AddressState aaaa{};
        CnameState cname{};
        bool has_nxdomain = false;
        std::chrono::steady_clock::time_point nxdomain_expire_at{};
    };

    static constexpr std::uint32_t kInvalidIndex = 0xffffffffU;
    static constexpr std::uint32_t kTombstoneIndex = 0xfffffffeU;

    [[nodiscard]] common::IoErr upsert_address(std::string_view qname,
                                               std::uint16_t qclass,
                                               const net::IpAddress *records,
                                               std::uint16_t count,
                                               std::chrono::steady_clock::time_point expire_at,
                                               RecordType type) noexcept;
    [[nodiscard]] common::IoErr normalize_name(std::string_view input,
                                               char *dst,
                                               std::size_t cap,
                                               std::string_view &out) const noexcept;
    [[nodiscard]] std::uint64_t hash_key(std::string_view name, std::uint16_t qclass) const noexcept;
    [[nodiscard]] std::uint32_t find_entry_index(std::string_view name,
                                                 std::uint16_t qclass,
                                                 std::uint64_t hash) const noexcept;
    [[nodiscard]] std::uint32_t find_insert_bucket(std::string_view name,
                                                   std::uint16_t qclass,
                                                   std::uint64_t hash) const noexcept;
    [[nodiscard]] std::string_view owner_name(const NameEntry &entry) const noexcept;
    [[nodiscard]] const net::IpAddress *address_records(const NameEntry &entry, const AddressSlot &slot) const noexcept;
    [[nodiscard]] std::string_view cname_target(const NameEntry &entry) const noexcept;

    void clear_address_slot(AddressSlot &slot) noexcept;
    void clear_cname_slot(CnameSlot &slot) noexcept;
    void cleanup_entry(NameEntry &entry, std::chrono::steady_clock::time_point now) noexcept;
    [[nodiscard]] bool entry_empty(const NameEntry &entry) const noexcept;
    void load_entry_state(const NameEntry &entry, EntryState &state) const noexcept;
    [[nodiscard]] common::IoErr store_entry_state(std::uint32_t index,
                                                  const EntryState &state,
                                                  bool is_new) noexcept;
    [[nodiscard]] common::IoErr ensure_capacity(std::uint32_t protected_index,
                                                std::size_t old_blob_size,
                                                std::size_t new_blob_size,
                                                bool need_new_entry) noexcept;
    [[nodiscard]] std::uint32_t allocate_entry() noexcept;
    void recycle_entry(std::uint32_t index) noexcept;
    void erase_entry(std::uint32_t index) noexcept;
    [[nodiscard]] std::uint32_t select_eviction_candidate(std::uint32_t protected_index) noexcept;
    void touch_entry(NameEntry &entry) noexcept;

    Options options_{};
    std::unique_ptr<std::uint32_t[]> buckets_{};
    std::unique_ptr<NameEntry[]> entries_{};
    std::size_t bucket_count_ = 0;
    std::size_t entry_count_ = 0;
    std::size_t bytes_used_ = 0;
    std::uint32_t free_head_ = kInvalidIndex;
    std::uint32_t eviction_cursor_ = 0;
    std::uint32_t sweep_cursor_ = 0;
    std::uint64_t access_clock_ = 0;
};

} // namespace fiber::dns

#endif // FIBER_DNS_DNS_CACHE_H
