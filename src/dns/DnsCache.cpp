#include "DnsCache.h"
#include "DnsName.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <type_traits>

#include "../common/Assert.h"

namespace fiber::dns {

namespace {

constexpr std::size_t kMaxDnsNameLen = 255;
constexpr std::size_t kWriteSweepBudget = 4;

std::size_t next_power_of_two(std::size_t value) noexcept {
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

std::size_t align_up(std::size_t value, std::size_t align) noexcept { return (value + align - 1U) & ~(align - 1U); }

bool has_deadline(std::chrono::steady_clock::time_point when) noexcept {
    return when.time_since_epoch() != std::chrono::steady_clock::duration::zero();
}

bool expired_at(std::chrono::steady_clock::time_point when, std::chrono::steady_clock::time_point now) noexcept {
    return has_deadline(when) && when <= now;
}

void reset_address_view(NameSnapshot::AddressView &view) noexcept { view = {}; }

} // namespace

static_assert(std::is_trivially_destructible_v<net::IpAddress>);

bool NameSnapshot::init(Options options) noexcept {
    release();
    options_ = options;
    if (options.max_name_storage == 0) {
        return false;
    }
    if (options.max_a_records != 0) {
        a_storage_ = std::make_unique<net::IpAddress[]>(options.max_a_records);
        if (!a_storage_) {
            release();
            return false;
        }
    }
    if (options.max_aaaa_records != 0) {
        aaaa_storage_ = std::make_unique<net::IpAddress[]>(options.max_aaaa_records);
        if (!aaaa_storage_) {
            release();
            return false;
        }
    }
    name_storage_ = std::make_unique<char[]>(options.max_name_storage);
    if (!name_storage_) {
        release();
        return false;
    }
    clear();
    return true;
}

void NameSnapshot::release() noexcept {
    a_storage_.reset();
    aaaa_storage_.reset();
    name_storage_.reset();
    options_ = {};
    clear();
}

void NameSnapshot::clear() noexcept {
    found_ = false;
    has_nxdomain_ = false;
    nxdomain_expire_at_ = {};
    reset_address_view(a_);
    reset_address_view(aaaa_);
    cname_ = {};
}

bool NameSnapshot::valid() const noexcept {
    return name_storage_ != nullptr && (options_.max_a_records == 0 || a_storage_ != nullptr) &&
           (options_.max_aaaa_records == 0 || aaaa_storage_ != nullptr);
}

common::IoErr NameSnapshot::assign_a(const net::IpAddress *records, std::uint16_t count, bool negative,
                                     std::chrono::steady_clock::time_point expire_at) noexcept {
    if (!negative && (records == nullptr || count == 0)) {
        return common::IoErr::Invalid;
    }
    if (count > options_.max_a_records) {
        return common::IoErr::NoMem;
    }
    for (std::uint16_t i = 0; i < count; ++i) {
        a_storage_[i] = records[i];
    }
    found_ = true;
    a_.records = count != 0 ? a_storage_.get() : nullptr;
    a_.count = count;
    a_.present = true;
    a_.negative = negative;
    a_.expire_at = expire_at;
    return common::IoErr::None;
}

common::IoErr NameSnapshot::assign_aaaa(const net::IpAddress *records, std::uint16_t count, bool negative,
                                        std::chrono::steady_clock::time_point expire_at) noexcept {
    if (!negative && (records == nullptr || count == 0)) {
        return common::IoErr::Invalid;
    }
    if (count > options_.max_aaaa_records) {
        return common::IoErr::NoMem;
    }
    for (std::uint16_t i = 0; i < count; ++i) {
        aaaa_storage_[i] = records[i];
    }
    found_ = true;
    aaaa_.records = count != 0 ? aaaa_storage_.get() : nullptr;
    aaaa_.count = count;
    aaaa_.present = true;
    aaaa_.negative = negative;
    aaaa_.expire_at = expire_at;
    return common::IoErr::None;
}

common::IoErr NameSnapshot::assign_cname(std::string_view target,
                                         std::chrono::steady_clock::time_point expire_at) noexcept {
    if (target.size() > options_.max_name_storage) {
        return common::IoErr::NoMem;
    }
    std::memcpy(name_storage_.get(), target.data(), target.size());
    found_ = true;
    cname_.target = std::string_view(name_storage_.get(), target.size());
    cname_.present = true;
    cname_.expire_at = expire_at;
    return common::IoErr::None;
}

void NameSnapshot::assign_nxdomain(std::chrono::steady_clock::time_point expire_at) noexcept {
    found_ = true;
    has_nxdomain_ = true;
    nxdomain_expire_at_ = expire_at;
}

bool DnsCache::init(Options options) noexcept {
    release();
    if (options.max_entries == 0 || options.max_entries > static_cast<std::size_t>(kTombstoneIndex) ||
        options.max_bytes == 0 || options.max_entries > std::numeric_limits<std::size_t>::max() / 2U) {
        return false;
    }
    const std::size_t min_index_capacity = next_power_of_two(options.max_entries * 2U);
    const std::size_t requested_index_capacity =
            options.index_capacity == 0 ? min_index_capacity : next_power_of_two(options.index_capacity);
    if (min_index_capacity == 0 || requested_index_capacity == 0) {
        return false;
    }
    options.index_capacity = std::max(min_index_capacity, requested_index_capacity);
    if (options.eviction_sample == 0) {
        options.eviction_sample = 1;
    }

    buckets_ = std::make_unique<std::uint32_t[]>(options.index_capacity);
    if (!buckets_) {
        return false;
    }
    entries_ = std::make_unique<NameEntry[]>(options.max_entries);
    if (!entries_) {
        buckets_.reset();
        return false;
    }

    for (std::size_t i = 0; i < options.index_capacity; ++i) {
        buckets_[i] = kInvalidIndex;
    }
    for (std::size_t i = 0; i < options.max_entries; ++i) {
        entries_[i].next_free = i + 1 < options.max_entries ? static_cast<std::uint32_t>(i + 1U) : kInvalidIndex;
    }

    options_ = options;
    bucket_count_ = options.index_capacity;
    free_head_ = 0;
    return true;
}

void DnsCache::release() noexcept {
    if (entries_) {
        for (std::size_t i = 0; i < options_.max_entries; ++i) {
            delete[] entries_[i].blob;
            entries_[i].blob = nullptr;
        }
    }
    buckets_.reset();
    entries_.reset();
    options_ = {};
    bucket_count_ = 0;
    entry_count_ = 0;
    bytes_used_ = 0;
    tombstone_count_ = 0;
    free_head_ = kInvalidIndex;
    eviction_cursor_ = 0;
    sweep_cursor_ = 0;
    access_clock_.store(0, std::memory_order_relaxed);
}

std::size_t DnsCache::entry_count() const noexcept { return entry_count_; }

std::size_t DnsCache::bytes_used() const noexcept { return bytes_used_; }

common::IoErr DnsCache::normalize_name(std::string_view input, char *dst, std::size_t cap,
                                       std::string_view &out) const noexcept {
    return dns::normalize_name(input, dst, cap, out);
}

std::uint64_t DnsCache::hash_key(std::string_view name, std::uint16_t qclass) const noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char ch: name) {
        hash ^= static_cast<std::uint64_t>(ch);
        hash *= 1099511628211ULL;
    }
    hash ^= static_cast<std::uint64_t>(qclass >> 8U);
    hash *= 1099511628211ULL;
    hash ^= static_cast<std::uint64_t>(qclass & 0xffU);
    hash *= 1099511628211ULL;
    return hash;
}

std::string_view DnsCache::owner_name(const NameEntry &entry) const noexcept {
    if (!entry.occupied || entry.blob == nullptr || entry.owner_len == 0) {
        return {};
    }
    return std::string_view(entry.blob, entry.owner_len);
}

const net::IpAddress *DnsCache::address_records(const NameEntry &entry, const AddressSlot &slot) const noexcept {
    if (!entry.occupied || entry.blob == nullptr || slot.state != SlotState::Positive || slot.count == 0) {
        return nullptr;
    }
    return reinterpret_cast<const net::IpAddress *>(entry.blob + slot.blob_offset);
}

std::string_view DnsCache::cname_target(const NameEntry &entry) const noexcept {
    if (!entry.occupied || entry.blob == nullptr || !entry.cname.present || entry.cname.target_len == 0) {
        return {};
    }
    return std::string_view(entry.blob + entry.cname.blob_offset, entry.cname.target_len);
}

std::uint32_t DnsCache::find_entry_index(std::string_view name, std::uint16_t qclass,
                                         std::uint64_t hash) const noexcept {
    if (!buckets_ || bucket_count_ == 0) {
        return kInvalidIndex;
    }
    std::size_t mask = bucket_count_ - 1U;
    std::size_t bucket = static_cast<std::size_t>(hash) & mask;
    for (std::size_t probes = 0; probes < bucket_count_; ++probes) {
        std::uint32_t index = buckets_[bucket];
        if (index == kInvalidIndex) {
            return kInvalidIndex;
        }
        if (index != kTombstoneIndex) {
            const NameEntry &entry = entries_[index];
            if (entry.occupied && entry.hash == hash && entry.qclass == qclass && owner_name(entry) == name) {
                return index;
            }
        }
        bucket = (bucket + 1U) & mask;
    }
    return kInvalidIndex;
}

std::uint32_t DnsCache::find_insert_bucket(std::string_view name, std::uint16_t qclass,
                                           std::uint64_t hash) const noexcept {
    std::size_t mask = bucket_count_ - 1U;
    std::size_t bucket = static_cast<std::size_t>(hash) & mask;
    std::uint32_t tombstone_bucket = kInvalidIndex;
    for (std::size_t probes = 0; probes < bucket_count_; ++probes) {
        std::uint32_t index = buckets_[bucket];
        if (index == kInvalidIndex) {
            return tombstone_bucket != kInvalidIndex ? tombstone_bucket : static_cast<std::uint32_t>(bucket);
        }
        if (index == kTombstoneIndex) {
            if (tombstone_bucket == kInvalidIndex) {
                tombstone_bucket = static_cast<std::uint32_t>(bucket);
            }
        } else {
            const NameEntry &entry = entries_[index];
            if (entry.occupied && entry.hash == hash && entry.qclass == qclass && owner_name(entry) == name) {
                return static_cast<std::uint32_t>(bucket);
            }
        }
        bucket = (bucket + 1U) & mask;
    }
    return tombstone_bucket;
}

void DnsCache::clear_address_slot(AddressSlot &slot) noexcept { slot = {}; }

void DnsCache::clear_cname_slot(CnameSlot &slot) noexcept { slot = {}; }

void DnsCache::cleanup_entry(NameEntry &entry, std::chrono::steady_clock::time_point now) noexcept {
    if (!entry.occupied) {
        return;
    }
    if (entry.a.state != SlotState::Empty && expired_at(entry.a.expire_at, now)) {
        clear_address_slot(entry.a);
    }
    if (entry.aaaa.state != SlotState::Empty && expired_at(entry.aaaa.expire_at, now)) {
        clear_address_slot(entry.aaaa);
    }
    if (entry.cname.present && expired_at(entry.cname.expire_at, now)) {
        clear_cname_slot(entry.cname);
    }
    if (has_deadline(entry.nxdomain_expire_at) && expired_at(entry.nxdomain_expire_at, now)) {
        entry.nxdomain_expire_at = {};
    }
}

bool DnsCache::entry_empty(const NameEntry &entry) const noexcept {
    return entry.a.state == SlotState::Empty && entry.aaaa.state == SlotState::Empty && !entry.cname.present &&
           !has_deadline(entry.nxdomain_expire_at);
}

void DnsCache::load_entry_state(const NameEntry &entry, EntryState &state) const noexcept {
    state = {};
    if (!entry.occupied) {
        return;
    }
    state.owner = owner_name(entry);
    state.nxdomain_expire_at = entry.nxdomain_expire_at;
    state.has_nxdomain = has_deadline(entry.nxdomain_expire_at);

    state.a.state = entry.a.state;
    state.a.count = entry.a.count;
    state.a.expire_at = entry.a.expire_at;
    state.a.records = address_records(entry, entry.a);

    state.aaaa.state = entry.aaaa.state;
    state.aaaa.count = entry.aaaa.count;
    state.aaaa.expire_at = entry.aaaa.expire_at;
    state.aaaa.records = address_records(entry, entry.aaaa);

    state.cname.present = entry.cname.present;
    state.cname.expire_at = entry.cname.expire_at;
    state.cname.target = cname_target(entry);
}

std::uint32_t DnsCache::allocate_entry() noexcept {
    if (free_head_ == kInvalidIndex) {
        return kInvalidIndex;
    }
    std::uint32_t index = free_head_;
    free_head_ = entries_[index].next_free;
    entries_[index].next_free = kInvalidIndex;
    entries_[index].occupied = true;
    ++entry_count_;
    return index;
}

void DnsCache::recycle_entry(std::uint32_t index) noexcept {
    NameEntry &entry = entries_[index];
    delete[] entry.blob;
    entry.blob = nullptr;
    bytes_used_ -= entry.blob_size;
    entry.hash = 0;
    entry.qclass = 0;
    entry.owner_len = 0;
    entry.blob_size = 0;
    entry.a = {};
    entry.aaaa = {};
    entry.cname = {};
    entry.nxdomain_expire_at = {};
    entry.approx_last_access.store(0, std::memory_order_relaxed);
    entry.occupied = false;
    entry.next_free = free_head_;
    free_head_ = index;
    --entry_count_;
}

void DnsCache::erase_entry(std::uint32_t index) noexcept {
    if (index == kInvalidIndex || !entries_[index].occupied) {
        return;
    }
    std::size_t mask = bucket_count_ - 1U;
    std::size_t bucket = static_cast<std::size_t>(entries_[index].hash) & mask;
    bool erased = false;
    for (std::size_t probes = 0; probes < bucket_count_; ++probes) {
        if (buckets_[bucket] == index) {
            buckets_[bucket] = kTombstoneIndex;
            ++tombstone_count_;
            erased = true;
            break;
        }
        bucket = (bucket + 1U) & mask;
    }
    FIBER_ASSERT(erased);
    recycle_entry(index);
}

void DnsCache::rebuild_index() noexcept {
    if (!buckets_ || bucket_count_ == 0) {
        return;
    }
    for (std::size_t i = 0; i < bucket_count_; ++i) {
        buckets_[i] = kInvalidIndex;
    }

    const std::size_t mask = bucket_count_ - 1U;
    for (std::size_t i = 0; i < options_.max_entries; ++i) {
        const NameEntry &entry = entries_[i];
        if (!entry.occupied || entry.blob == nullptr || entry.owner_len == 0) {
            continue;
        }
        std::size_t bucket = static_cast<std::size_t>(entry.hash) & mask;
        bool inserted = false;
        for (std::size_t probes = 0; probes < bucket_count_; ++probes) {
            if (buckets_[bucket] == kInvalidIndex) {
                buckets_[bucket] = static_cast<std::uint32_t>(i);
                inserted = true;
                break;
            }
            bucket = (bucket + 1U) & mask;
        }
        FIBER_ASSERT(inserted);
    }
    tombstone_count_ = 0;
}

void DnsCache::maybe_rebuild_index() noexcept {
    if (tombstone_count_ == 0 || bucket_count_ == 0) {
        return;
    }
    const std::size_t threshold = std::max<std::size_t>(1, bucket_count_ / 4U);
    if (tombstone_count_ >= threshold) {
        rebuild_index();
    }
}

void DnsCache::touch_entry(const NameEntry &entry) const noexcept {
    const std::uint64_t next = access_clock_.fetch_add(1, std::memory_order_relaxed) + 1U;
    entry.approx_last_access.store(next, std::memory_order_relaxed);
}

std::uint32_t DnsCache::select_eviction_candidate(std::uint32_t protected_index) noexcept {
    if (entry_count_ == 0) {
        return kInvalidIndex;
    }

    std::uint32_t best = kInvalidIndex;
    std::uint64_t best_access = std::numeric_limits<std::uint64_t>::max();
    std::size_t sampled = 0;
    for (std::size_t i = 0; i < options_.max_entries && sampled < options_.eviction_sample; ++i) {
        std::uint32_t index = static_cast<std::uint32_t>((eviction_cursor_ + i) % options_.max_entries);
        const NameEntry &entry = entries_[index];
        if (!entry.occupied || index == protected_index) {
            continue;
        }
        ++sampled;
        const std::uint64_t last_access = entry.approx_last_access.load(std::memory_order_relaxed);
        if (last_access < best_access) {
            best_access = last_access;
            best = index;
        }
    }
    eviction_cursor_ = static_cast<std::uint32_t>((eviction_cursor_ + options_.eviction_sample) % options_.max_entries);
    if (best != kInvalidIndex) {
        return best;
    }

    for (std::size_t i = 0; i < options_.max_entries; ++i) {
        const NameEntry &entry = entries_[i];
        if (!entry.occupied || static_cast<std::uint32_t>(i) == protected_index) {
            continue;
        }
        const std::uint64_t last_access = entry.approx_last_access.load(std::memory_order_relaxed);
        if (best == kInvalidIndex || last_access < best_access) {
            best = static_cast<std::uint32_t>(i);
            best_access = last_access;
        }
    }
    return best;
}

common::IoErr DnsCache::ensure_capacity(std::uint32_t protected_index, std::size_t old_blob_size,
                                        std::size_t new_blob_size, bool need_new_entry) noexcept {
    if (new_blob_size > options_.max_bytes) {
        return common::IoErr::NoMem;
    }

    for (;;) {
        bool entries_ok = !need_new_entry || free_head_ != kInvalidIndex;
        bool bytes_ok = bytes_used_ - old_blob_size + new_blob_size <= options_.max_bytes;
        if (entries_ok && bytes_ok) {
            return common::IoErr::None;
        }

        std::uint32_t victim = select_eviction_candidate(protected_index);
        if (victim == kInvalidIndex) {
            return common::IoErr::NoMem;
        }
        erase_entry(victim);
    }
}

common::IoErr DnsCache::store_entry_state(std::uint32_t index, const EntryState &state, bool is_new) noexcept {
    if (state.owner.empty()) {
        if (!is_new) {
            erase_entry(index);
        }
        return common::IoErr::None;
    }

    std::size_t blob_size = state.owner.size();
    std::uint32_t cname_offset = 0;
    std::uint32_t a_offset = 0;
    std::uint32_t aaaa_offset = 0;

    if (state.cname.present) {
        cname_offset = static_cast<std::uint32_t>(blob_size);
        blob_size += state.cname.target.size();
    }
    if (state.a.state == SlotState::Positive) {
        blob_size = align_up(blob_size, alignof(net::IpAddress));
        a_offset = static_cast<std::uint32_t>(blob_size);
        blob_size += static_cast<std::size_t>(state.a.count) * sizeof(net::IpAddress);
    }
    if (state.aaaa.state == SlotState::Positive) {
        blob_size = align_up(blob_size, alignof(net::IpAddress));
        aaaa_offset = static_cast<std::uint32_t>(blob_size);
        blob_size += static_cast<std::size_t>(state.aaaa.count) * sizeof(net::IpAddress);
    }
    if (blob_size > std::numeric_limits<std::uint32_t>::max()) {
        return common::IoErr::NoMem;
    }

    NameEntry &entry = entries_[index];
    common::IoErr cap_err = ensure_capacity(index, entry.blob_size, blob_size, false);
    if (cap_err != common::IoErr::None) {
        return cap_err;
    }

    char *new_blob = new (std::nothrow) char[blob_size == 0 ? 1 : blob_size];
    if (!new_blob) {
        return common::IoErr::NoMem;
    }

    std::memcpy(new_blob, state.owner.data(), state.owner.size());
    if (state.cname.present) {
        std::memcpy(new_blob + cname_offset, state.cname.target.data(), state.cname.target.size());
    }
    if (state.a.state == SlotState::Positive) {
        auto *dst = reinterpret_cast<net::IpAddress *>(new_blob + a_offset);
        for (std::uint16_t i = 0; i < state.a.count; ++i) {
            std::construct_at(dst + i, state.a.records[i]);
        }
    }
    if (state.aaaa.state == SlotState::Positive) {
        auto *dst = reinterpret_cast<net::IpAddress *>(new_blob + aaaa_offset);
        for (std::uint16_t i = 0; i < state.aaaa.count; ++i) {
            std::construct_at(dst + i, state.aaaa.records[i]);
        }
    }

    delete[] entry.blob;
    bytes_used_ = bytes_used_ - entry.blob_size + blob_size;

    entry.hash = hash_key(state.owner, entry.qclass);
    entry.owner_len = static_cast<std::uint16_t>(state.owner.size());
    entry.blob_size = static_cast<std::uint32_t>(blob_size);
    entry.blob = new_blob;

    entry.a.state = state.a.state;
    entry.a.count = state.a.state == SlotState::Positive ? state.a.count : 0;
    entry.a.blob_offset = state.a.state == SlotState::Positive ? a_offset : 0;
    entry.a.expire_at = state.a.state != SlotState::Empty ? state.a.expire_at : std::chrono::steady_clock::time_point{};

    entry.aaaa.state = state.aaaa.state;
    entry.aaaa.count = state.aaaa.state == SlotState::Positive ? state.aaaa.count : 0;
    entry.aaaa.blob_offset = state.aaaa.state == SlotState::Positive ? aaaa_offset : 0;
    entry.aaaa.expire_at =
            state.aaaa.state != SlotState::Empty ? state.aaaa.expire_at : std::chrono::steady_clock::time_point{};

    entry.cname.present = state.cname.present;
    entry.cname.target_len = state.cname.present ? static_cast<std::uint16_t>(state.cname.target.size()) : 0;
    entry.cname.blob_offset = state.cname.present ? cname_offset : 0;
    entry.cname.expire_at = state.cname.present ? state.cname.expire_at : std::chrono::steady_clock::time_point{};

    entry.nxdomain_expire_at = state.has_nxdomain ? state.nxdomain_expire_at : std::chrono::steady_clock::time_point{};
    touch_entry(entry);
    return common::IoErr::None;
}

common::IoErr DnsCache::fill_snapshot(const NameEntry &entry, std::chrono::steady_clock::time_point now,
                                      NameSnapshot &out) const noexcept {
    common::IoErr err = common::IoErr::None;
    if (entry.a.state == SlotState::Positive && !expired_at(entry.a.expire_at, now)) {
        err = out.assign_a(address_records(entry, entry.a), entry.a.count, false, entry.a.expire_at);
        if (err != common::IoErr::None) {
            return err;
        }
    } else if (entry.a.state == SlotState::NegativeNoData && !expired_at(entry.a.expire_at, now)) {
        err = out.assign_a(nullptr, 0, true, entry.a.expire_at);
        if (err != common::IoErr::None) {
            return err;
        }
    }

    if (entry.aaaa.state == SlotState::Positive && !expired_at(entry.aaaa.expire_at, now)) {
        err = out.assign_aaaa(address_records(entry, entry.aaaa), entry.aaaa.count, false, entry.aaaa.expire_at);
        if (err != common::IoErr::None) {
            return err;
        }
    } else if (entry.aaaa.state == SlotState::NegativeNoData && !expired_at(entry.aaaa.expire_at, now)) {
        err = out.assign_aaaa(nullptr, 0, true, entry.aaaa.expire_at);
        if (err != common::IoErr::None) {
            return err;
        }
    }

    if (entry.cname.present && !expired_at(entry.cname.expire_at, now)) {
        err = out.assign_cname(cname_target(entry), entry.cname.expire_at);
        if (err != common::IoErr::None) {
            return err;
        }
    }
    if (has_deadline(entry.nxdomain_expire_at) && !expired_at(entry.nxdomain_expire_at, now)) {
        out.assign_nxdomain(entry.nxdomain_expire_at);
    }
    return common::IoErr::None;
}

common::IoErr DnsCache::peek_name(std::string_view qname, std::uint16_t qclass,
                                  std::chrono::steady_clock::time_point now, NameSnapshot &out) const noexcept {
    out.clear();
    if (!out.valid() || qclass == 0) {
        return common::IoErr::Invalid;
    }

    char normalized_buf[kMaxDnsNameLen + 1];
    std::string_view normalized;
    common::IoErr err = normalize_name(qname, normalized_buf, sizeof(normalized_buf), normalized);
    if (err != common::IoErr::None) {
        return err;
    }

    std::uint64_t hash = hash_key(normalized, qclass);
    std::uint32_t index = find_entry_index(normalized, qclass, hash);
    if (index == kInvalidIndex) {
        return common::IoErr::None;
    }

    return fill_snapshot(entries_[index], now, out);
}

common::IoErr DnsCache::lookup_name_shared(std::string_view qname, std::uint16_t qclass,
                                           std::chrono::steady_clock::time_point now, NameSnapshot &out,
                                           SharedLookupState &state) const noexcept {
    out.clear();
    state = SharedLookupState::Miss;
    if (!out.valid() || qclass == 0) {
        return common::IoErr::Invalid;
    }

    char normalized_buf[kMaxDnsNameLen + 1];
    std::string_view normalized;
    common::IoErr err = normalize_name(qname, normalized_buf, sizeof(normalized_buf), normalized);
    if (err != common::IoErr::None) {
        return err;
    }

    const std::uint64_t hash = hash_key(normalized, qclass);
    const std::uint32_t index = find_entry_index(normalized, qclass, hash);
    if (index == kInvalidIndex) {
        return common::IoErr::None;
    }

    const NameEntry &entry = entries_[index];
    err = fill_snapshot(entry, now, out);
    if (err != common::IoErr::None) {
        return err;
    }
    if (!out.found()) {
        state = SharedLookupState::Expired;
        return common::IoErr::None;
    }

    touch_entry(entry);
    state = SharedLookupState::Hit;
    return common::IoErr::None;
}

common::IoErr DnsCache::lookup_name(std::string_view qname, std::uint16_t qclass,
                                    std::chrono::steady_clock::time_point now, NameSnapshot &out) noexcept {
    out.clear();
    if (!out.valid() || qclass == 0) {
        return common::IoErr::Invalid;
    }

    char normalized_buf[kMaxDnsNameLen + 1];
    std::string_view normalized;
    common::IoErr err = normalize_name(qname, normalized_buf, sizeof(normalized_buf), normalized);
    if (err != common::IoErr::None) {
        return err;
    }

    std::uint64_t hash = hash_key(normalized, qclass);
    std::uint32_t index = find_entry_index(normalized, qclass, hash);
    if (index == kInvalidIndex) {
        return common::IoErr::None;
    }

    NameEntry &entry = entries_[index];
    cleanup_entry(entry, now);
    if (entry_empty(entry)) {
        erase_entry(index);
        maybe_rebuild_index();
        return common::IoErr::None;
    }

    err = fill_snapshot(entry, now, out);
    if (err != common::IoErr::None) {
        return err;
    }
    touch_entry(entry);
    return common::IoErr::None;
}

common::IoErr DnsCache::upsert_address(std::string_view qname, std::uint16_t qclass, const net::IpAddress *records,
                                       std::uint16_t count, std::chrono::steady_clock::time_point expire_at,
                                       RecordType type) noexcept {
    if (qclass == 0 || records == nullptr || count == 0 || !has_deadline(expire_at)) {
        return common::IoErr::Invalid;
    }
    for (std::uint16_t i = 0; i < count; ++i) {
        if ((type == RecordType::A && !records[i].is_v4()) || (type == RecordType::AAAA && !records[i].is_v6())) {
            return common::IoErr::Invalid;
        }
    }

    char normalized_buf[kMaxDnsNameLen + 1];
    std::string_view normalized;
    common::IoErr err = normalize_name(qname, normalized_buf, sizeof(normalized_buf), normalized);
    if (err != common::IoErr::None) {
        return err;
    }

    std::uint64_t hash = hash_key(normalized, qclass);
    std::uint32_t index = find_entry_index(normalized, qclass, hash);
    bool is_new = false;
    if (index == kInvalidIndex) {
        index = allocate_entry();
        if (index == kInvalidIndex) {
            common::IoErr cap_err = ensure_capacity(kInvalidIndex, 0, normalized.size(), true);
            if (cap_err != common::IoErr::None) {
                return cap_err;
            }
            index = allocate_entry();
            if (index == kInvalidIndex) {
                return common::IoErr::NoMem;
            }
        }
        NameEntry &entry = entries_[index];
        entry.qclass = qclass;
        entry.hash = hash;
        is_new = true;
    }

    EntryState state;
    load_entry_state(entries_[index], state);
    state.owner = normalized;
    state.has_nxdomain = false;
    state.nxdomain_expire_at = {};
    if (type == RecordType::A) {
        state.a.state = SlotState::Positive;
        state.a.records = records;
        state.a.count = count;
        state.a.expire_at = expire_at;
    } else {
        state.aaaa.state = SlotState::Positive;
        state.aaaa.records = records;
        state.aaaa.count = count;
        state.aaaa.expire_at = expire_at;
    }

    err = store_entry_state(index, state, is_new);
    if (err != common::IoErr::None) {
        if (is_new) {
            recycle_entry(index);
        }
        return err;
    }
    if (is_new) {
        std::uint32_t bucket = find_insert_bucket(normalized, qclass, hash);
        if (bucket == kInvalidIndex) {
            recycle_entry(index);
            return common::IoErr::NoMem;
        }
        if (buckets_[bucket] == kTombstoneIndex) {
            FIBER_ASSERT(tombstone_count_ != 0);
            --tombstone_count_;
        }
        buckets_[bucket] = index;
    }
    maybe_rebuild_index();
    return common::IoErr::None;
}

common::IoErr DnsCache::upsert_a(std::string_view qname, std::uint16_t qclass, const net::IpAddress *records,
                                 std::uint16_t count, std::chrono::steady_clock::time_point expire_at) noexcept {
    return upsert_address(qname, qclass, records, count, expire_at, RecordType::A);
}

common::IoErr DnsCache::upsert_aaaa(std::string_view qname, std::uint16_t qclass, const net::IpAddress *records,
                                    std::uint16_t count, std::chrono::steady_clock::time_point expire_at) noexcept {
    return upsert_address(qname, qclass, records, count, expire_at, RecordType::AAAA);
}

common::IoErr DnsCache::upsert_cname(std::string_view qname, std::uint16_t qclass, std::string_view target,
                                     std::chrono::steady_clock::time_point expire_at) noexcept {
    if (qclass == 0 || !has_deadline(expire_at)) {
        return common::IoErr::Invalid;
    }

    char normalized_buf[kMaxDnsNameLen + 1];
    char target_buf[kMaxDnsNameLen + 1];
    std::string_view normalized;
    std::string_view normalized_target;
    common::IoErr err = normalize_name(qname, normalized_buf, sizeof(normalized_buf), normalized);
    if (err != common::IoErr::None) {
        return err;
    }
    err = normalize_name(target, target_buf, sizeof(target_buf), normalized_target);
    if (err != common::IoErr::None) {
        return err;
    }

    std::uint64_t hash = hash_key(normalized, qclass);
    std::uint32_t index = find_entry_index(normalized, qclass, hash);
    bool is_new = false;
    if (index == kInvalidIndex) {
        index = allocate_entry();
        if (index == kInvalidIndex) {
            common::IoErr cap_err =
                    ensure_capacity(kInvalidIndex, 0, normalized.size() + normalized_target.size(), true);
            if (cap_err != common::IoErr::None) {
                return cap_err;
            }
            index = allocate_entry();
            if (index == kInvalidIndex) {
                return common::IoErr::NoMem;
            }
        }
        NameEntry &entry = entries_[index];
        entry.qclass = qclass;
        entry.hash = hash;
        is_new = true;
    }

    EntryState state;
    load_entry_state(entries_[index], state);
    state.owner = normalized;
    state.has_nxdomain = false;
    state.nxdomain_expire_at = {};
    state.cname.present = true;
    state.cname.target = normalized_target;
    state.cname.expire_at = expire_at;

    err = store_entry_state(index, state, is_new);
    if (err != common::IoErr::None) {
        if (is_new) {
            recycle_entry(index);
        }
        return err;
    }
    if (is_new) {
        std::uint32_t bucket = find_insert_bucket(normalized, qclass, hash);
        if (bucket == kInvalidIndex) {
            recycle_entry(index);
            return common::IoErr::NoMem;
        }
        if (buckets_[bucket] == kTombstoneIndex) {
            FIBER_ASSERT(tombstone_count_ != 0);
            --tombstone_count_;
        }
        buckets_[bucket] = index;
    }
    maybe_rebuild_index();
    return common::IoErr::None;
}

common::IoErr DnsCache::upsert_negative_nxdomain(std::string_view qname, std::uint16_t qclass,
                                                 std::chrono::steady_clock::time_point expire_at) noexcept {
    if (qclass == 0 || !has_deadline(expire_at)) {
        return common::IoErr::Invalid;
    }

    char normalized_buf[kMaxDnsNameLen + 1];
    std::string_view normalized;
    common::IoErr err = normalize_name(qname, normalized_buf, sizeof(normalized_buf), normalized);
    if (err != common::IoErr::None) {
        return err;
    }

    std::uint64_t hash = hash_key(normalized, qclass);
    std::uint32_t index = find_entry_index(normalized, qclass, hash);
    bool is_new = false;
    if (index == kInvalidIndex) {
        index = allocate_entry();
        if (index == kInvalidIndex) {
            common::IoErr cap_err = ensure_capacity(kInvalidIndex, 0, normalized.size(), true);
            if (cap_err != common::IoErr::None) {
                return cap_err;
            }
            index = allocate_entry();
            if (index == kInvalidIndex) {
                return common::IoErr::NoMem;
            }
        }
        NameEntry &entry = entries_[index];
        entry.qclass = qclass;
        entry.hash = hash;
        is_new = true;
    }

    // NXDOMAIN is keyed by (qname, qclass), unlike NODATA, which is keyed by
    // (qname, qtype, qclass). Replace all RRsets for this owner so a snapshot
    // never contains contradictory positive/CNAME and NXDOMAIN state.
    EntryState state;
    state.owner = normalized;
    state.has_nxdomain = true;
    state.nxdomain_expire_at = expire_at;

    err = store_entry_state(index, state, is_new);
    if (err != common::IoErr::None) {
        if (is_new) {
            recycle_entry(index);
        }
        return err;
    }
    if (is_new) {
        std::uint32_t bucket = find_insert_bucket(normalized, qclass, hash);
        if (bucket == kInvalidIndex) {
            recycle_entry(index);
            return common::IoErr::NoMem;
        }
        if (buckets_[bucket] == kTombstoneIndex) {
            FIBER_ASSERT(tombstone_count_ != 0);
            --tombstone_count_;
        }
        buckets_[bucket] = index;
    }
    maybe_rebuild_index();
    return common::IoErr::None;
}

common::IoErr DnsCache::upsert_negative_nodata(std::string_view qname, std::uint16_t qclass, std::uint16_t qtype,
                                               std::chrono::steady_clock::time_point expire_at) noexcept {
    if (qclass == 0 || !has_deadline(expire_at)) {
        return common::IoErr::Invalid;
    }
    if (qtype != static_cast<std::uint16_t>(RecordType::A) && qtype != static_cast<std::uint16_t>(RecordType::AAAA)) {
        return common::IoErr::NotSupported;
    }

    char normalized_buf[kMaxDnsNameLen + 1];
    std::string_view normalized;
    common::IoErr err = normalize_name(qname, normalized_buf, sizeof(normalized_buf), normalized);
    if (err != common::IoErr::None) {
        return err;
    }

    std::uint64_t hash = hash_key(normalized, qclass);
    std::uint32_t index = find_entry_index(normalized, qclass, hash);
    bool is_new = false;
    if (index == kInvalidIndex) {
        index = allocate_entry();
        if (index == kInvalidIndex) {
            common::IoErr cap_err = ensure_capacity(kInvalidIndex, 0, normalized.size(), true);
            if (cap_err != common::IoErr::None) {
                return cap_err;
            }
            index = allocate_entry();
            if (index == kInvalidIndex) {
                return common::IoErr::NoMem;
            }
        }
        NameEntry &entry = entries_[index];
        entry.qclass = qclass;
        entry.hash = hash;
        is_new = true;
    }

    EntryState state;
    load_entry_state(entries_[index], state);
    state.owner = normalized;
    state.has_nxdomain = false;
    state.nxdomain_expire_at = {};
    if (qtype == static_cast<std::uint16_t>(RecordType::A)) {
        state.a.state = SlotState::NegativeNoData;
        state.a.records = nullptr;
        state.a.count = 0;
        state.a.expire_at = expire_at;
    } else {
        state.aaaa.state = SlotState::NegativeNoData;
        state.aaaa.records = nullptr;
        state.aaaa.count = 0;
        state.aaaa.expire_at = expire_at;
    }

    err = store_entry_state(index, state, is_new);
    if (err != common::IoErr::None) {
        if (is_new) {
            recycle_entry(index);
        }
        return err;
    }
    if (is_new) {
        std::uint32_t bucket = find_insert_bucket(normalized, qclass, hash);
        if (bucket == kInvalidIndex) {
            recycle_entry(index);
            return common::IoErr::NoMem;
        }
        if (buckets_[bucket] == kTombstoneIndex) {
            FIBER_ASSERT(tombstone_count_ != 0);
            --tombstone_count_;
        }
        buckets_[bucket] = index;
    }
    maybe_rebuild_index();
    return common::IoErr::None;
}

common::IoErr DnsCache::erase(std::string_view qname, std::uint16_t qclass) noexcept {
    if (qclass == 0) {
        return common::IoErr::Invalid;
    }
    char normalized_buf[kMaxDnsNameLen + 1];
    std::string_view normalized;
    common::IoErr err = normalize_name(qname, normalized_buf, sizeof(normalized_buf), normalized);
    if (err != common::IoErr::None) {
        return err;
    }
    std::uint32_t index = find_entry_index(normalized, qclass, hash_key(normalized, qclass));
    if (index != kInvalidIndex) {
        erase_entry(index);
        maybe_rebuild_index();
    }
    return common::IoErr::None;
}

std::size_t DnsCache::sweep_expired(std::chrono::steady_clock::time_point now, std::size_t scan_budget) noexcept {
    if (scan_budget == 0 || options_.max_entries == 0) {
        return 0;
    }
    std::size_t removed = 0;
    std::size_t scanned = 0;
    while (scanned < options_.max_entries && scanned < scan_budget) {
        std::uint32_t index = sweep_cursor_ % options_.max_entries;
        sweep_cursor_ = static_cast<std::uint32_t>((sweep_cursor_ + 1U) % options_.max_entries);
        ++scanned;
        NameEntry &entry = entries_[index];
        if (!entry.occupied) {
            continue;
        }
        cleanup_entry(entry, now);
        if (entry_empty(entry)) {
            erase_entry(index);
            ++removed;
        }
    }
    maybe_rebuild_index();
    return removed;
}

async::Task<std::size_t> SharedDnsCache::entry_count() noexcept {
    auto guard = co_await mutex_.lock_shared();
    co_return cache_.entry_count();
}

async::Task<std::size_t> SharedDnsCache::bytes_used() noexcept {
    auto guard = co_await mutex_.lock_shared();
    co_return cache_.bytes_used();
}

async::Task<common::IoErr> SharedDnsCache::lookup_name(std::string_view qname, std::uint16_t qclass,
                                                       std::chrono::steady_clock::time_point now,
                                                       NameSnapshot &out) noexcept {
    common::IoErr err;
    DnsCache::SharedLookupState state;
    {
        auto guard = co_await mutex_.lock_shared();
        err = cache_.lookup_name_shared(qname, qclass, now, out, state);
    }

    if (err != common::IoErr::None || state != DnsCache::SharedLookupState::Expired) {
        co_return err;
    }

    auto guard = co_await mutex_.lock();
    co_return cache_.lookup_name(qname, qclass, now, out);
}

async::Task<common::IoErr> SharedDnsCache::upsert_a(std::string_view qname, std::uint16_t qclass,
                                                    const net::IpAddress *records, std::uint16_t count,
                                                    std::chrono::steady_clock::time_point expire_at) noexcept {
    auto guard = co_await mutex_.lock();
    (void) cache_.sweep_expired(event::EventLoop::current().now(), kWriteSweepBudget);
    co_return cache_.upsert_a(qname, qclass, records, count, expire_at);
}

async::Task<common::IoErr> SharedDnsCache::upsert_aaaa(std::string_view qname, std::uint16_t qclass,
                                                       const net::IpAddress *records, std::uint16_t count,
                                                       std::chrono::steady_clock::time_point expire_at) noexcept {
    auto guard = co_await mutex_.lock();
    (void) cache_.sweep_expired(event::EventLoop::current().now(), kWriteSweepBudget);
    co_return cache_.upsert_aaaa(qname, qclass, records, count, expire_at);
}

async::Task<common::IoErr> SharedDnsCache::upsert_cname(std::string_view qname, std::uint16_t qclass,
                                                        std::string_view target,
                                                        std::chrono::steady_clock::time_point expire_at) noexcept {
    auto guard = co_await mutex_.lock();
    (void) cache_.sweep_expired(event::EventLoop::current().now(), kWriteSweepBudget);
    co_return cache_.upsert_cname(qname, qclass, target, expire_at);
}

async::Task<common::IoErr>
SharedDnsCache::upsert_negative_nxdomain(std::string_view qname, std::uint16_t qclass,
                                         std::chrono::steady_clock::time_point expire_at) noexcept {
    auto guard = co_await mutex_.lock();
    (void) cache_.sweep_expired(event::EventLoop::current().now(), kWriteSweepBudget);
    co_return cache_.upsert_negative_nxdomain(qname, qclass, expire_at);
}

async::Task<common::IoErr>
SharedDnsCache::upsert_negative_nodata(std::string_view qname, std::uint16_t qclass, std::uint16_t qtype,
                                       std::chrono::steady_clock::time_point expire_at) noexcept {
    auto guard = co_await mutex_.lock();
    (void) cache_.sweep_expired(event::EventLoop::current().now(), kWriteSweepBudget);
    co_return cache_.upsert_negative_nodata(qname, qclass, qtype, expire_at);
}

async::Task<common::IoErr> SharedDnsCache::erase(std::string_view qname, std::uint16_t qclass) noexcept {
    auto guard = co_await mutex_.lock();
    co_return cache_.erase(qname, qclass);
}

async::Task<std::size_t> SharedDnsCache::sweep_expired(std::chrono::steady_clock::time_point now,
                                                       std::size_t scan_budget) noexcept {
    auto guard = co_await mutex_.lock();
    co_return cache_.sweep_expired(now, scan_budget);
}

} // namespace fiber::dns
