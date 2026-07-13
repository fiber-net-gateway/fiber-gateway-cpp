#include "DnsResolverLocal.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <expected>
#include <limits>

#include "../common/Assert.h"
#include "../net/UdpSocket.h"
#include "DnsName.h"

namespace fiber::dns {

namespace {

constexpr std::size_t kDnsHeaderSize = 12;
constexpr std::size_t kMaxDnsNameLen = 255;

std::uint32_t read_be32(const std::uint8_t *data) noexcept {
    return (static_cast<std::uint32_t>(data[0]) << 24U) | (static_cast<std::uint32_t>(data[1]) << 16U) |
           (static_cast<std::uint32_t>(data[2]) << 8U) | static_cast<std::uint32_t>(data[3]);
}

ResolveStatus map_error_rcode(RCode rcode) noexcept {
    switch (rcode) {
        case RCode::FormatError:
            return ResolveStatus::FormatError;
        case RCode::ServerFailure:
            return ResolveStatus::ServerFailure;
        case RCode::NotImplemented:
            return ResolveStatus::NotImplemented;
        case RCode::Refused:
            return ResolveStatus::Refused;
        case RCode::NoError:
        case RCode::NxDomain:
            break;
    }
    return ResolveStatus::ServerFailure;
}

std::chrono::seconds clamp_ttl(std::uint32_t ttl, std::chrono::seconds min_ttl, std::chrono::seconds max_ttl) noexcept {
    auto clamped = std::chrono::seconds(ttl);
    if (clamped < min_ttl) {
        clamped = min_ttl;
    }
    if (clamped > max_ttl) {
        clamped = max_ttl;
    }
    return clamped;
}

std::chrono::steady_clock::time_point ttl_deadline(std::chrono::steady_clock::time_point now, std::uint32_t ttl,
                                                   std::chrono::seconds min_ttl,
                                                   std::chrono::seconds max_ttl) noexcept {
    return now + clamp_ttl(ttl, min_ttl, max_ttl);
}

bool is_supported_type(std::uint16_t qtype) noexcept {
    return qtype == static_cast<std::uint16_t>(RecordType::A) || qtype == static_cast<std::uint16_t>(RecordType::AAAA);
}

bool parse_ipv4(const MessageParser::ResourceRecord &record, net::IpAddress &out) noexcept {
    if (record.rdata_len != 4) {
        return false;
    }
    std::array<std::uint8_t, 4> bytes{};
    std::memcpy(bytes.data(), record.rdata, bytes.size());
    out = net::IpAddress::v4(bytes);
    return true;
}

bool parse_ipv6(const MessageParser::ResourceRecord &record, net::IpAddress &out) noexcept {
    if (record.rdata_len != 16) {
        return false;
    }
    std::array<std::uint8_t, 16> bytes{};
    std::memcpy(bytes.data(), record.rdata, bytes.size());
    out = net::IpAddress::v6(bytes);
    return true;
}

common::IoResult<std::uint32_t> parse_soa_negative_ttl(const MessageParser::MessageView &message,
                                                       const MessageParser::ResourceRecord &record) noexcept {
    if (record.type != static_cast<std::uint16_t>(RecordType::SOA) || record.rdata == nullptr) {
        return std::unexpected(common::IoErr::Invalid);
    }

    std::array<char, kMaxDnsNameLen + 1> scratch{};
    auto mname =
            decode_name(message.packet_data, message.packet_len, record.rdata_offset, scratch.data(), scratch.size());
    if (!mname) {
        return std::unexpected(mname.error());
    }
    auto rname =
            decode_name(message.packet_data, message.packet_len, mname->next_offset, scratch.data(), scratch.size());
    if (!rname) {
        return std::unexpected(rname.error());
    }
    if (rname->next_offset + 20 > message.packet_len) {
        return std::unexpected(common::IoErr::Invalid);
    }

    const std::uint8_t *tail = message.packet_data + rname->next_offset;
    const std::uint32_t minimum = read_be32(tail + 16);
    return std::min(record.ttl, minimum);
}

common::IoResult<std::uint32_t> find_negative_ttl(const MessageParser::MessageView &message,
                                                  std::uint16_t qclass) noexcept {
    for (std::uint16_t i = 0; i < message.authority_count; ++i) {
        const auto &record = message.authorities[i];
        if (record.type != static_cast<std::uint16_t>(RecordType::SOA) || record.dns_class != qclass) {
            continue;
        }
        auto ttl = parse_soa_negative_ttl(message, record);
        if (ttl) {
            return ttl;
        }
    }
    return std::unexpected(common::IoErr::NotFound);
}

enum class AnswerSetKind : std::uint8_t {
    None,
    Address,
    Cname,
};

struct AnswerSetView {
    AnswerSetKind kind = AnswerSetKind::None;
    std::uint16_t address_count = 0;
    std::uint32_t ttl = 0;
    std::string_view cname_target{};
};

common::IoErr inspect_answer_set(const MessageParser::MessageView &message, std::string_view owner, std::uint16_t qtype,
                                 std::uint16_t qclass, char *target_storage, std::size_t target_storage_cap,
                                 AnswerSetView &out) noexcept {
    out = {};
    std::array<char, kMaxDnsNameLen + 1> owner_buf{};
    std::array<char, kMaxDnsNameLen + 1> decoded_target_buf{};
    std::array<char, kMaxDnsNameLen + 1> other_target_buf{};
    bool saw_qtype = false;
    bool saw_cname = false;
    bool target_set = false;
    std::uint32_t address_ttl = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t cname_ttl = std::numeric_limits<std::uint32_t>::max();

    for (std::uint16_t i = 0; i < message.answer_count; ++i) {
        const MessageParser::ResourceRecord &record = message.answers[i];
        if (record.dns_class != qclass) {
            continue;
        }

        std::string_view normalized_owner;
        common::IoErr err = normalize_name(record.name, owner_buf.data(), owner_buf.size(), normalized_owner);
        if (err != common::IoErr::None) {
            return err;
        }
        if (normalized_owner != owner) {
            continue;
        }

        if (record.type == qtype) {
            saw_qtype = true;
            net::IpAddress address;
            const bool parsed = qtype == static_cast<std::uint16_t>(RecordType::A) ? parse_ipv4(record, address)
                                                                                   : parse_ipv6(record, address);
            if (parsed) {
                ++out.address_count;
                address_ttl = std::min(address_ttl, record.ttl);
            }
            continue;
        }

        if (record.type != static_cast<std::uint16_t>(RecordType::CNAME)) {
            continue;
        }

        saw_cname = true;
        auto decoded = decode_name(message.packet_data, message.packet_len, record.rdata_offset,
                                   decoded_target_buf.data(), decoded_target_buf.size());
        if (!decoded || decoded->next_offset != record.rdata_offset + record.rdata_len) {
            return common::IoErr::Invalid;
        }

        std::string_view normalized_target;
        char *storage = target_set ? other_target_buf.data() : target_storage;
        const std::size_t storage_cap = target_set ? other_target_buf.size() : target_storage_cap;
        err = normalize_name(decoded->name, storage, storage_cap, normalized_target);
        if (err != common::IoErr::None) {
            return err;
        }
        if (target_set && normalized_target != out.cname_target) {
            return common::IoErr::Invalid;
        }
        if (!target_set) {
            out.cname_target = normalized_target;
            target_set = true;
        }
        cname_ttl = std::min(cname_ttl, record.ttl);
    }

    if (saw_qtype && saw_cname) {
        return common::IoErr::Invalid;
    }
    if (saw_qtype) {
        if (out.address_count == 0) {
            return common::IoErr::Invalid;
        }
        out.kind = AnswerSetKind::Address;
        out.ttl = address_ttl;
        return common::IoErr::None;
    }
    if (saw_cname) {
        if (!target_set || out.cname_target == owner) {
            return common::IoErr::Invalid;
        }
        out.kind = AnswerSetKind::Cname;
        out.ttl = cname_ttl;
    }
    return common::IoErr::None;
}

} // namespace

bool ResolveResult::init(Options options) noexcept {
    release();
    options_ = options;
    if (options.max_name_storage == 0) {
        return false;
    }
    if (options.max_records != 0) {
        records_ = std::make_unique<net::IpAddress[]>(options.max_records);
        if (!records_) {
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

void ResolveResult::release() noexcept {
    records_.reset();
    name_storage_.reset();
    options_ = {};
    clear();
}

void ResolveResult::clear() noexcept {
    canonical_name_ = {};
    record_count_ = 0;
    expire_at_ = {};
}

bool ResolveResult::valid() const noexcept {
    return name_storage_ != nullptr && (options_.max_records == 0 || records_ != nullptr);
}

common::IoErr ResolveResult::assign_positive(std::string_view canonical_name, const net::IpAddress *records,
                                             std::uint16_t count,
                                             std::chrono::steady_clock::time_point expire_at) noexcept {
    if ((count != 0 && records == nullptr) || count > options_.max_records) {
        return count > options_.max_records ? common::IoErr::NoMem : common::IoErr::Invalid;
    }
    common::IoErr err = assign_canonical(canonical_name);
    if (err != common::IoErr::None) {
        return err;
    }
    for (std::uint16_t i = 0; i < count; ++i) {
        records_[i] = records[i];
    }
    record_count_ = count;
    expire_at_ = expire_at;
    return common::IoErr::None;
}

common::IoErr ResolveResult::assign_canonical(std::string_view canonical_name) noexcept {
    if (canonical_name.size() > options_.max_name_storage) {
        return common::IoErr::NoMem;
    }
    std::memcpy(name_storage_.get(), canonical_name.data(), canonical_name.size());
    canonical_name_ = std::string_view(name_storage_.get(), canonical_name.size());
    return common::IoErr::None;
}

DnsResolverLocal::PendingAwaiter::PendingAwaiter(DnsResolverLocal &resolver, std::uint16_t pending_index) noexcept :
    resolver_(&resolver), pending_index_(pending_index) {}

DnsResolverLocal::PendingAwaiter::~PendingAwaiter() {
    if (resolver_ && queued_) {
        resolver_->cancel_waiter(pending_index_, &waiter_);
    }
}

bool DnsResolverLocal::PendingAwaiter::await_suspend(std::coroutine_handle<> handle) noexcept {
    FIBER_ASSERT(resolver_ != nullptr);
    waiter_.handle = handle;
    queued_ = resolver_->enqueue_waiter(pending_index_, &waiter_);
    return queued_;
}

DnsResolverLocal::PendingOutcome DnsResolverLocal::PendingAwaiter::await_resume() noexcept {
    queued_ = false;
    return waiter_.outcome;
}

DnsResolverLocal::~DnsResolverLocal() {
    if (valid()) {
        FIBER_ASSERT(loop_ != nullptr);
        FIBER_ASSERT(loop_->in_loop());
        close();
    }
}

bool DnsResolverLocal::init(event::EventLoop &loop, SharedDnsCache &cache, DnsClient::Options client_options,
                            Options options) noexcept {
    release();
    if (options.max_cname_hops == 0 || options.max_pending == 0 || options.max_packet_size < kDnsHeaderSize ||
        options.min_positive_ttl > options.max_positive_ttl || options.min_negative_ttl > options.max_negative_ttl) {
        return false;
    }

    options_ = options;
    loop_ = &loop;
    cache_ = &cache;
    closing_ = false;
    if (!init_pending_storage()) {
        release();
        return false;
    }
    if (!client_.init(loop, client_options)) {
        release();
        return false;
    }
    return true;
}

void DnsResolverLocal::close() noexcept {
    if (!loop_) {
        return;
    }
    FIBER_ASSERT(loop_->in_loop());
    if (closing_) {
        return;
    }
    closing_ = true;
    cancel_all_pending(common::IoErr::Canceled);
    client_.close();
}

void DnsResolverLocal::release() noexcept {
    if (client_.valid()) {
        FIBER_ASSERT(loop_ != nullptr);
        FIBER_ASSERT(loop_->in_loop());
        close();
    }
    client_.release();
    release_pending_storage();
    reset_state();
}

bool DnsResolverLocal::valid() const noexcept { return loop_ != nullptr && cache_ != nullptr && client_.valid(); }

event::EventLoop &DnsResolverLocal::loop() const noexcept {
    FIBER_ASSERT(loop_ != nullptr);
    return *loop_;
}

bool DnsResolverLocal::init_pending_storage() noexcept {
    pending_ = std::make_unique<PendingEntry[]>(options_.max_pending);
    packet_storage_ =
            std::make_unique<std::uint8_t[]>(static_cast<std::size_t>(options_.max_pending) * options_.max_packet_size);
    if (!pending_ || !packet_storage_) {
        return false;
    }

    for (std::size_t i = 0; i < options_.max_pending; ++i) {
        PendingEntry &entry = pending_[i];
        entry.active = false;
        entry.qtype = 0;
        entry.qclass = 0;
        entry.next_free = i + 1 < options_.max_pending ? static_cast<std::uint16_t>(i + 1) : kInvalidPending;
        entry.name_len = 0;
        entry.name[0] = '\0';
        entry.packet_buf = packet_storage_.get() + (i * options_.max_packet_size);
        entry.waiters = nullptr;
        if (!entry.parser.init(options_.parser_options)) {
            return false;
        }
    }
    free_head_ = 0;
    return true;
}

void DnsResolverLocal::release_pending_storage() noexcept {
    if (pending_) {
        for (std::size_t i = 0; i < options_.max_pending; ++i) {
            pending_[i].parser.release();
        }
    }
    packet_storage_.reset();
    pending_.reset();
}

void DnsResolverLocal::reset_state() noexcept {
    options_ = {};
    loop_ = nullptr;
    cache_ = nullptr;
    free_head_ = kInvalidPending;
    closing_ = false;
}

void DnsResolverLocal::cancel_all_pending(common::IoErr err) noexcept {
    if (!pending_) {
        return;
    }
    for (std::size_t i = 0; i < options_.max_pending; ++i) {
        PendingEntry &entry = pending_[i];
        if (!entry.active) {
            continue;
        }
        PendingOutcome outcome{};
        outcome.err = err;
        finish_pending(static_cast<std::uint16_t>(i), outcome);
    }
}

common::IoErr DnsResolverLocal::normalize_name(std::string_view input, char *dst, std::size_t cap,
                                               std::string_view &out) const noexcept {
    return dns::normalize_name(input, dst, cap, out);
}

std::uint16_t DnsResolverLocal::find_pending(std::string_view qname, std::uint16_t qtype,
                                             std::uint16_t qclass) const noexcept {
    if (!pending_) {
        return kInvalidPending;
    }
    for (std::size_t i = 0; i < options_.max_pending; ++i) {
        const PendingEntry &entry = pending_[i];
        if (!entry.active || entry.qtype != qtype || entry.qclass != qclass ||
            entry.name_len != static_cast<std::uint16_t>(qname.size())) {
            continue;
        }
        if (std::memcmp(entry.name, qname.data(), qname.size()) == 0) {
            return static_cast<std::uint16_t>(i);
        }
    }
    return kInvalidPending;
}

std::uint16_t DnsResolverLocal::allocate_pending(std::string_view qname, std::uint16_t qtype,
                                                 std::uint16_t qclass) noexcept {
    if (free_head_ == kInvalidPending || qname.size() > kMaxNormalizedNameLen) {
        return kInvalidPending;
    }
    const std::uint16_t index = free_head_;
    PendingEntry &entry = pending_[index];
    free_head_ = entry.next_free;
    entry.active = true;
    entry.qtype = qtype;
    entry.qclass = qclass;
    entry.name_len = static_cast<std::uint16_t>(qname.size());
    std::memcpy(entry.name, qname.data(), qname.size());
    entry.name[qname.size()] = '\0';
    entry.waiters = nullptr;
    entry.next_free = kInvalidPending;
    return index;
}

void DnsResolverLocal::release_pending(std::uint16_t index) noexcept {
    FIBER_ASSERT(index < options_.max_pending);
    PendingEntry &entry = pending_[index];
    entry.active = false;
    entry.qtype = 0;
    entry.qclass = 0;
    entry.name_len = 0;
    entry.name[0] = '\0';
    entry.waiters = nullptr;
    entry.next_free = free_head_;
    free_head_ = index;
}

bool DnsResolverLocal::enqueue_waiter(std::uint16_t index, PendingWaiter *waiter) noexcept {
    if (!pending_ || index >= options_.max_pending || waiter == nullptr) {
        return false;
    }
    PendingEntry &entry = pending_[index];
    if (!entry.active) {
        return false;
    }
    waiter->next = entry.waiters;
    entry.waiters = waiter;
    return true;
}

void DnsResolverLocal::cancel_waiter(std::uint16_t index, PendingWaiter *waiter) noexcept {
    if (!pending_ || index >= options_.max_pending || waiter == nullptr) {
        return;
    }
    PendingEntry &entry = pending_[index];
    PendingWaiter **link = &entry.waiters;
    while (*link) {
        if (*link == waiter) {
            *link = waiter->next;
            waiter->next = nullptr;
            return;
        }
        link = &(*link)->next;
    }
}

void DnsResolverLocal::finish_pending(std::uint16_t index, PendingOutcome outcome) noexcept {
    if (!pending_ || index >= options_.max_pending) {
        return;
    }
    PendingEntry &entry = pending_[index];
    FIBER_ASSERT(entry.active);
    PendingWaiter *waiter = entry.waiters;
    entry.waiters = nullptr;

    // A resumed waiter may immediately reuse this slot, so do not access entry after releasing it.
    release_pending(index);

    while (waiter) {
        PendingWaiter *next = waiter->next;
        waiter->next = nullptr;
        waiter->outcome = outcome;
        if (waiter->handle) {
            waiter->handle.resume();
        }
        waiter = next;
    }
}

DnsResolverLocal::CacheLookup DnsResolverLocal::lookup_snapshot(const NameSnapshot &snapshot,
                                                                std::uint16_t qtype) const noexcept {
    CacheLookup out{};
    const NameSnapshot::AddressView &view =
            qtype == static_cast<std::uint16_t>(RecordType::A) ? snapshot.a() : snapshot.aaaa();

    if (view.present) {
        out.kind = view.negative ? CacheLookupKind::NoData : CacheLookupKind::Positive;
        out.records = view.records;
        out.count = view.count;
        out.expire_at = view.expire_at;
        return out;
    }
    if (snapshot.cname().present) {
        out.kind = CacheLookupKind::Cname;
        out.cname_target = snapshot.cname().target;
        out.expire_at = snapshot.cname().expire_at;
        return out;
    }
    if (snapshot.has_nxdomain()) {
        out.kind = CacheLookupKind::NxDomain;
        out.expire_at = snapshot.nxdomain_expire_at();
        return out;
    }
    return out;
}

common::IoResult<ResolveStatus> DnsResolverLocal::finish_from_cache(std::string_view canonical_name,
                                                                    const CacheLookup &lookup,
                                                                    ResolveResult &out) noexcept {
    common::IoErr err = common::IoErr::None;
    switch (lookup.kind) {
        case CacheLookupKind::Positive:
            err = out.assign_positive(canonical_name, lookup.records, lookup.count, lookup.expire_at);
            if (err != common::IoErr::None) {
                return std::unexpected(err);
            }
            return ResolveStatus::Success;
        case CacheLookupKind::NxDomain:
            err = out.assign_canonical(canonical_name);
            if (err != common::IoErr::None) {
                return std::unexpected(err);
            }
            return ResolveStatus::NxDomain;
        case CacheLookupKind::NoData:
            err = out.assign_canonical(canonical_name);
            if (err != common::IoErr::None) {
                return std::unexpected(err);
            }
            return ResolveStatus::NoData;
        case CacheLookupKind::Miss:
        case CacheLookupKind::Cname:
            break;
    }
    return std::unexpected(common::IoErr::Invalid);
}

async::Task<common::IoResult<ResolveStatus>> DnsResolverLocal::resolve(const QuestionSpec &question,
                                                                       ResolveResult &out) noexcept {
    FIBER_ASSERT(loop_ != nullptr);
    FIBER_ASSERT(loop_->in_loop());

    out.clear();
    if (!valid() || closing_) {
        co_return std::unexpected(common::IoErr::Canceled);
    }
    if (!out.valid() || question.dns_class != static_cast<std::uint16_t>(RecordClass::IN) ||
        !is_supported_type(question.type)) {
        co_return std::unexpected(question.dns_class != static_cast<std::uint16_t>(RecordClass::IN) ||
                                                  !is_supported_type(question.type)
                                          ? common::IoErr::NotSupported
                                          : common::IoErr::Invalid);
    }

    NameSnapshot snapshot;
    if (!snapshot.init(options_.snapshot_options)) {
        co_return std::unexpected(common::IoErr::NoMem);
    }

    std::array<char, kMaxNormalizedNameLen + 1> current_name_buf{};
    std::array<char, kMaxNormalizedNameLen + 1> next_name_buf{};
    std::string_view current_name;
    common::IoErr err = normalize_name(question.name, current_name_buf.data(), current_name_buf.size(), current_name);
    if (err != common::IoErr::None) {
        co_return std::unexpected(err);
    }

    std::uint16_t cname_hops = 0;
    bool expect_cache_hit = false;
    for (;;) {
        snapshot.clear();
        auto cache_err = co_await cache_->lookup_name(current_name, question.dns_class, loop_->now(), snapshot);
        if (cache_err != common::IoErr::None) {
            co_return std::unexpected(cache_err);
        }

        CacheLookup lookup = lookup_snapshot(snapshot, question.type);
        if (lookup.kind == CacheLookupKind::Positive || lookup.kind == CacheLookupKind::NxDomain ||
            lookup.kind == CacheLookupKind::NoData) {
            co_return finish_from_cache(current_name, lookup, out);
        }

        if (lookup.kind == CacheLookupKind::Cname) {
            if (cname_hops >= options_.max_cname_hops) {
                co_return std::unexpected(common::IoErr::Invalid);
            }
            ++cname_hops;
            err = normalize_name(lookup.cname_target, next_name_buf.data(), next_name_buf.size(), current_name);
            if (err != common::IoErr::None) {
                co_return std::unexpected(err);
            }
            expect_cache_hit = false;
            continue;
        }

        if (expect_cache_hit) {
            co_return std::unexpected(common::IoErr::Invalid);
        }

        const std::uint16_t existing_pending = find_pending(current_name, question.type, question.dns_class);
        if (existing_pending != kInvalidPending) {
            PendingOutcome outcome = co_await PendingAwaiter(*this, existing_pending);
            if (outcome.err != common::IoErr::None) {
                co_return std::unexpected(outcome.err);
            }
            if (outcome.action == PendingAction::ReturnStatus) {
                err = out.assign_canonical(current_name);
                if (err != common::IoErr::None) {
                    co_return std::unexpected(err);
                }
                co_return outcome.status;
            }
            expect_cache_hit = true;
            continue;
        }

        const std::uint16_t pending_index = allocate_pending(current_name, question.type, question.dns_class);
        if (pending_index == kInvalidPending) {
            co_return std::unexpected(common::IoErr::Busy);
        }

        PendingEntry &pending = pending_[pending_index];
        PendingOutcome outcome{};
        auto upstream = co_await query_upstream(current_name, question.type, question.dns_class, pending);
        if (!upstream) {
            outcome.err = upstream.error();
        } else {
            outcome = *upstream;
        }
        finish_pending(pending_index, outcome);

        if (outcome.err != common::IoErr::None) {
            co_return std::unexpected(outcome.err);
        }
        if (outcome.action == PendingAction::ReturnStatus) {
            err = out.assign_canonical(current_name);
            if (err != common::IoErr::None) {
                co_return std::unexpected(err);
            }
            co_return outcome.status;
        }
        expect_cache_hit = true;
    }
}

async::Task<common::IoResult<DnsResolverLocal::PendingOutcome>>
DnsResolverLocal::query_upstream(std::string_view qname, std::uint16_t qtype, std::uint16_t qclass,
                                 PendingEntry &pending) noexcept {
    QuestionSpec question{};
    question.name = qname;
    question.type = qtype;
    question.dns_class = qclass;

    auto query_result = co_await client_.query_raw(question, pending.packet_buf, options_.max_packet_size);
    if (!query_result) {
        co_return std::unexpected(query_result.error());
    }

    co_return co_await handle_response(qname, qtype, qclass, pending.packet_buf, *query_result, pending.parser);
}

async::Task<common::IoResult<DnsResolverLocal::PendingOutcome>>
DnsResolverLocal::handle_response(std::string_view qname, std::uint16_t qtype, std::uint16_t qclass,
                                  const std::uint8_t *packet, std::size_t packet_len, MessageParser &parser) noexcept {
    auto parsed = parser.parse(packet, packet_len);
    if (!parsed) {
        co_return std::unexpected(parsed.error());
    }

    const MessageParser::MessageView &message = *parsed;
    if (!message.header.is_response() || message.question_count != 1 || message.questions == nullptr) {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    std::array<char, kMaxDnsNameLen + 1> question_name_buf{};
    std::string_view normalized_question_name;
    common::IoErr err = normalize_name(message.questions[0].name, question_name_buf.data(), question_name_buf.size(),
                                       normalized_question_name);
    if (err != common::IoErr::None) {
        co_return std::unexpected(err);
    }
    if (normalized_question_name != qname || message.questions[0].type != qtype ||
        message.questions[0].dns_class != qclass) {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    PendingOutcome outcome{};
    const RCode rcode = message.header.rcode();
    if (rcode != RCode::NoError && rcode != RCode::NxDomain) {
        outcome.status = map_error_rcode(rcode);
        co_return outcome;
    }

    const auto now = loop_->now();
    if (rcode == RCode::NxDomain) {
        auto neg_ttl = find_negative_ttl(message, qclass);
        if (neg_ttl) {
            err = co_await cache_->upsert_negative_nxdomain(
                    qname, qclass, ttl_deadline(now, *neg_ttl, options_.min_negative_ttl, options_.max_negative_ttl));
            if (err != common::IoErr::None) {
                co_return std::unexpected(err);
            }
            outcome.action = PendingAction::RetryFromCache;
            co_return outcome;
        }
        outcome.status = ResolveStatus::NxDomain;
        co_return outcome;
    }

    std::array<char, kMaxDnsNameLen + 1> first_chain_name_buf{};
    std::array<char, kMaxDnsNameLen + 1> second_chain_name_buf{};
    std::string_view chain_owner = qname;
    bool use_first_chain_buf = true;
    bool has_relevant_answer = false;
    bool has_terminal_address = false;
    std::uint16_t response_cname_hops = 0;

    for (;;) {
        char *target_storage = use_first_chain_buf ? first_chain_name_buf.data() : second_chain_name_buf.data();
        const std::size_t target_storage_cap =
                use_first_chain_buf ? first_chain_name_buf.size() : second_chain_name_buf.size();
        AnswerSetView answer_set;
        err = inspect_answer_set(message, chain_owner, qtype, qclass, target_storage, target_storage_cap, answer_set);
        if (err != common::IoErr::None) {
            co_return std::unexpected(err);
        }
        if (answer_set.kind == AnswerSetKind::Address) {
            has_relevant_answer = true;
            has_terminal_address = true;
            break;
        }
        if (answer_set.kind != AnswerSetKind::Cname) {
            break;
        }
        if (response_cname_hops >= options_.max_cname_hops) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
        ++response_cname_hops;
        has_relevant_answer = true;
        chain_owner = answer_set.cname_target;
        use_first_chain_buf = !use_first_chain_buf;
    }

    if (!has_relevant_answer) {
        if (message.answer_count != 0) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
        auto neg_ttl = find_negative_ttl(message, qclass);
        if (neg_ttl) {
            err = co_await cache_->upsert_negative_nodata(
                    qname, qclass, qtype,
                    ttl_deadline(now, *neg_ttl, options_.min_negative_ttl, options_.max_negative_ttl));
            if (err != common::IoErr::None) {
                co_return std::unexpected(err);
            }
            outcome.action = PendingAction::RetryFromCache;
            co_return outcome;
        }
        outcome.status = ResolveStatus::NoData;
        co_return outcome;
    }

    std::unique_ptr<net::IpAddress[]> temp_records;
    if (has_terminal_address) {
        temp_records = std::make_unique<net::IpAddress[]>(message.answer_count);
        if (!temp_records) {
            co_return std::unexpected(common::IoErr::NoMem);
        }
    }

    chain_owner = qname;
    use_first_chain_buf = true;
    std::uint16_t applied_cname_hops = 0;
    for (;;) {
        char *target_storage = use_first_chain_buf ? first_chain_name_buf.data() : second_chain_name_buf.data();
        const std::size_t target_storage_cap =
                use_first_chain_buf ? first_chain_name_buf.size() : second_chain_name_buf.size();
        AnswerSetView answer_set;
        err = inspect_answer_set(message, chain_owner, qtype, qclass, target_storage, target_storage_cap, answer_set);
        if (err != common::IoErr::None) {
            co_return std::unexpected(err);
        }

        if (answer_set.kind == AnswerSetKind::Cname) {
            if (applied_cname_hops >= response_cname_hops) {
                co_return std::unexpected(common::IoErr::Invalid);
            }
            err = co_await cache_->upsert_cname(
                    chain_owner, qclass, answer_set.cname_target,
                    ttl_deadline(now, answer_set.ttl, options_.min_positive_ttl, options_.max_positive_ttl));
            if (err != common::IoErr::None) {
                co_return std::unexpected(err);
            }
            ++applied_cname_hops;
            chain_owner = answer_set.cname_target;
            use_first_chain_buf = !use_first_chain_buf;
            continue;
        }

        if (answer_set.kind == AnswerSetKind::Address) {
            std::array<char, kMaxDnsNameLen + 1> owner_buf{};
            std::uint16_t count = 0;
            for (std::uint16_t i = 0; i < message.answer_count; ++i) {
                const MessageParser::ResourceRecord &record = message.answers[i];
                if (record.type != qtype || record.dns_class != qclass) {
                    continue;
                }
                std::string_view normalized_owner;
                err = normalize_name(record.name, owner_buf.data(), owner_buf.size(), normalized_owner);
                if (err != common::IoErr::None) {
                    co_return std::unexpected(err);
                }
                if (normalized_owner != chain_owner) {
                    continue;
                }

                net::IpAddress address;
                const bool parsed = qtype == static_cast<std::uint16_t>(RecordType::A) ? parse_ipv4(record, address)
                                                                                       : parse_ipv6(record, address);
                if (parsed) {
                    temp_records[count++] = address;
                }
            }
            if (count != answer_set.address_count) {
                co_return std::unexpected(common::IoErr::Invalid);
            }

            const auto expire_at =
                    ttl_deadline(now, answer_set.ttl, options_.min_positive_ttl, options_.max_positive_ttl);
            if (qtype == static_cast<std::uint16_t>(RecordType::A)) {
                err = co_await cache_->upsert_a(chain_owner, qclass, temp_records.get(), count, expire_at);
            } else {
                err = co_await cache_->upsert_aaaa(chain_owner, qclass, temp_records.get(), count, expire_at);
            }
            if (err != common::IoErr::None) {
                co_return std::unexpected(err);
            }
        }
        break;
    }

    outcome.action = PendingAction::RetryFromCache;
    co_return outcome;
}

} // namespace fiber::dns
