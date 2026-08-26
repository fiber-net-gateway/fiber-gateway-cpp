#include <fiber/dns/DnsResolverLocal.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <expected>
#include <limits>
#include <new>

#include <fiber/common/Assert.h>
#include <fiber/dns/DnsName.h>
#include <fiber/net/UdpSocket.h>

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

common::IoResult<std::uint32_t> parse_soa_negative_ttl(const MessageParser::ScannedMessageView &message,
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
    const std::size_t rdata_end = record.rdata_offset + record.rdata_len;
    if (mname->next_offset > rdata_end) {
        return std::unexpected(common::IoErr::Invalid);
    }
    auto rname =
            decode_name(message.packet_data, message.packet_len, mname->next_offset, scratch.data(), scratch.size());
    if (!rname) {
        return std::unexpected(rname.error());
    }
    if (rname->next_offset > rdata_end || rdata_end - rname->next_offset != 20) {
        return std::unexpected(common::IoErr::Invalid);
    }

    const std::uint8_t *tail = message.packet_data + rname->next_offset;
    const std::uint32_t minimum = read_be32(tail + 16);
    return std::min(record.ttl, minimum);
}

struct NegativeAuthorityView {
    bool has_soa_ttl = false;
    bool saw_ns = false;
    std::uint32_t soa_ttl = 0;
};

common::IoResult<NegativeAuthorityView> inspect_negative_authority(const MessageParser::ScannedMessageView &message,
                                                                   std::uint16_t qclass) noexcept {
    NegativeAuthorityView out;
    MessageParser::RecordCursor cursor = MessageParser::cursor(message.authorities);
    std::array<char, kMaxDnsNameLen + 1> record_name_storage{};
    for (;;) {
        MessageParser::ResourceRecord record;
        auto next = MessageParser::next_record(message, cursor, record_name_storage.data(), record_name_storage.size(),
                                               record);
        if (!next) {
            return std::unexpected(next.error());
        }
        if (!*next) {
            return out;
        }
        if (record.dns_class != qclass) {
            continue;
        }
        if (record.type == static_cast<std::uint16_t>(RecordType::NS)) {
            out.saw_ns = true;
            continue;
        }
        if (out.has_soa_ttl || record.type != static_cast<std::uint16_t>(RecordType::SOA)) {
            continue;
        }
        auto ttl = parse_soa_negative_ttl(message, record);
        if (ttl) {
            out.has_soa_ttl = true;
            out.soa_ttl = *ttl;
        }
    }
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

common::IoErr inspect_answer_set(const MessageParser::ScannedMessageView &message, std::string_view owner,
                                 std::uint16_t qtype, std::uint16_t qclass, char *target_storage,
                                 std::size_t target_storage_cap, AnswerSetView &out) noexcept {
    out = {};
    std::array<char, kMaxDnsNameLen + 1> record_name_storage{};
    std::array<char, kMaxDnsNameLen + 1> owner_buf{};
    std::array<char, kMaxDnsNameLen + 1> decoded_target_buf{};
    std::array<char, kMaxDnsNameLen + 1> other_target_buf{};
    bool saw_qtype = false;
    bool saw_cname = false;
    bool target_set = false;
    std::uint32_t address_ttl = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t cname_ttl = std::numeric_limits<std::uint32_t>::max();

    MessageParser::RecordCursor cursor = MessageParser::cursor(message.answers);
    for (;;) {
        MessageParser::ResourceRecord record;
        auto next = MessageParser::next_record(message, cursor, record_name_storage.data(), record_name_storage.size(),
                                               record);
        if (!next) {
            return next.error();
        }
        if (!*next) {
            break;
        }
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
            if (!parsed) {
                return common::IoErr::Invalid;
            }
            ++out.address_count;
            address_ttl = std::min(address_ttl, record.ttl);
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

void ResolveResult::clear() noexcept {
    addresses_ = {};
    canonical_name_[0] = '\0';
    canonical_name_len_ = 0;
    expire_at_ = {};
}

common::IoErr ResolveResult::assign_positive(std::string_view canonical_name, const net::IpAddress *records,
                                             std::uint16_t count,
                                             std::chrono::steady_clock::time_point expire_at) noexcept {
    if ((count != 0 && records == nullptr) || count > kDnsMaxAddressesPerFamily) {
        return count > kDnsMaxAddressesPerFamily ? common::IoErr::MessageTooLarge : common::IoErr::Invalid;
    }
    common::IoErr err = assign_canonical(canonical_name);
    if (err != common::IoErr::None) {
        return err;
    }
    std::uint16_t v4_count = 0;
    for (std::uint16_t i = 0; i < count; ++i) {
        if (i != 0 && records[i].family() != records[0].family()) {
            clear();
            return common::IoErr::Invalid;
        }
        addresses_.records[i] = records[i];
        v4_count += records[i].is_v4() ? 1 : 0;
    }
    addresses_.count = count;
    addresses_.v4_count = v4_count;
    expire_at_ = expire_at;
    return common::IoErr::None;
}

common::IoErr ResolveResult::assign_canonical(std::string_view canonical_name) noexcept {
    if (canonical_name.size() >= sizeof(canonical_name_)) {
        return common::IoErr::MessageTooLarge;
    }
    std::memcpy(canonical_name_, canonical_name.data(), canonical_name.size());
    canonical_name_[canonical_name.size()] = '\0';
    canonical_name_len_ = static_cast<std::uint16_t>(canonical_name.size());
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

bool DnsResolverLocal::init(event::EventLoop &loop, SharedDnsCache2 &cache, DnsClient::Options client_options,
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
    if (!parser_.init(options_.parser_options)) {
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
    client_.close();
    cancel_all_pending(common::IoErr::Canceled);
}

void DnsResolverLocal::release() noexcept {
    if (client_.valid()) {
        FIBER_ASSERT(loop_ != nullptr);
        FIBER_ASSERT(loop_->in_loop());
        close();
    }
    client_.release();
    parser_.release();
    release_pending_storage();
    reset_state();
}

bool DnsResolverLocal::valid() const noexcept { return loop_ != nullptr && cache_ != nullptr && client_.valid(); }

event::EventLoop &DnsResolverLocal::loop() const noexcept {
    FIBER_ASSERT(loop_ != nullptr);
    return *loop_;
}

bool DnsResolverLocal::init_pending_storage() noexcept {
    pending_.reset(new (std::nothrow) PendingEntry[options_.max_pending]);
    packet_storage_.reset(
            new (std::nothrow) std::uint8_t[static_cast<std::size_t>(options_.max_pending) * options_.max_packet_size]);
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
    }
    free_head_ = 0;
    return true;
}

void DnsResolverLocal::release_pending_storage() noexcept {
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

async::Task<common::IoResult<ResolveStatus>> DnsResolverLocal::resolve(const QuestionSpec &question,
                                                                       ResolveResult &out) noexcept {
    FIBER_ASSERT(loop_ != nullptr);
    FIBER_ASSERT(loop_->in_loop());

    out.clear();
    if (!valid() || closing_) {
        co_return std::unexpected(common::IoErr::Canceled);
    }
    if (question.dns_class != static_cast<std::uint16_t>(RecordClass::IN) || !is_supported_type(question.type)) {
        co_return std::unexpected(common::IoErr::NotSupported);
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
        DnsCacheOut cached;
        const DnsCacheKey cache_key{current_name, dns_cache_hash(current_name)};
        const common::IoErr cache_err = cache_->lookup(cache_key, loop_->now(), cached);
        if (cache_err != common::IoErr::None) {
            co_return std::unexpected(cache_err);
        }

        if (cached.kind == DnsCacheOutKind::Addresses) {
            const DnsCacheAddressOut &addresses = cached.value.addresses;
            const bool query_v4 = question.type == static_cast<std::uint16_t>(RecordType::A);
            const bool family_cached = query_v4 ? addresses.has_v4() : addresses.has_v6();
            if (family_cached) {
                const std::uint16_t offset = query_v4 ? 0 : addresses.address_set.v4_count;
                const std::uint16_t count = query_v4 ? addresses.address_set.v4_count
                                                     : static_cast<std::uint16_t>(addresses.address_set.count -
                                                                                  addresses.address_set.v4_count);
                if (count == 0) {
                    err = out.assign_canonical(current_name);
                    if (err != common::IoErr::None) {
                        co_return std::unexpected(err);
                    }
                    co_return ResolveStatus::NoData;
                }
                const auto expire_at = query_v4 ? addresses.v4_expire_at : addresses.v6_expire_at;
                err = out.assign_positive(current_name, addresses.address_set.records + offset, count, expire_at);
                if (err != common::IoErr::None) {
                    co_return std::unexpected(err);
                }
                co_return ResolveStatus::Success;
            }
        }

        if (cached.kind == DnsCacheOutKind::NxDomain) {
            err = out.assign_canonical(current_name);
            if (err != common::IoErr::None) {
                co_return std::unexpected(err);
            }
            co_return ResolveStatus::NxDomain;
        }

        if (cached.kind == DnsCacheOutKind::Cname) {
            if (cname_hops >= options_.max_cname_hops) {
                co_return std::unexpected(common::IoErr::Invalid);
            }
            ++cname_hops;
            const std::string_view target(cached.value.cname.buf, cached.value.cname.length);
            err = normalize_name(target, next_name_buf.data(), next_name_buf.size(), current_name);
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

    const DnsClient::ResponseValidator validator{.context = this, .validate = &validate_upstream_response};
    auto query_result = co_await client_.query_raw(question, pending.packet_buf, options_.max_packet_size, validator);
    if (!query_result) {
        co_return std::unexpected(query_result.error());
    }

    co_return handle_response(qname, qtype, qclass, pending.packet_buf, *query_result);
}

DnsClient::ResponseDisposition DnsResolverLocal::validate_upstream_response(void *context, const std::uint8_t *packet,
                                                                            std::size_t packet_len) noexcept {
    FIBER_ASSERT(context != nullptr);
    auto &resolver = *static_cast<DnsResolverLocal *>(context);
    auto parsed = resolver.parser_.scan(packet, packet_len);
    if (!parsed) {
        return parsed.error() == common::IoErr::Invalid ? DnsClient::ResponseDisposition::RetryServer
                                                        : DnsClient::ResponseDisposition::Accept;
    }
    if (!parsed->header.is_response() || parsed->question_count != 1 || parsed->questions == nullptr) {
        return DnsClient::ResponseDisposition::RetryServer;
    }
    return DnsClient::ResponseDisposition::Accept;
}

common::IoResult<DnsResolverLocal::PendingOutcome>
DnsResolverLocal::handle_response(std::string_view qname, std::uint16_t qtype, std::uint16_t qclass,
                                  const std::uint8_t *packet, std::size_t packet_len) noexcept {
    auto parsed = parser_.scan(packet, packet_len);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }

    const MessageParser::ScannedMessageView &message = *parsed;
    if (!message.header.is_response() || message.question_count != 1 || message.questions == nullptr) {
        return std::unexpected(common::IoErr::Invalid);
    }

    std::array<char, kMaxDnsNameLen + 1> question_name_buf{};
    std::string_view normalized_question_name;
    common::IoErr err = normalize_name(message.questions[0].name, question_name_buf.data(), question_name_buf.size(),
                                       normalized_question_name);
    if (err != common::IoErr::None) {
        return std::unexpected(err);
    }
    if (normalized_question_name != qname || message.questions[0].type != qtype ||
        message.questions[0].dns_class != qclass) {
        return std::unexpected(common::IoErr::Invalid);
    }

    PendingOutcome outcome{};
    const RCode rcode = message.header.rcode();
    if (rcode != RCode::NoError && rcode != RCode::NxDomain) {
        outcome.status = map_error_rcode(rcode);
        return outcome;
    }

    const auto now = loop_->now();
    if (rcode == RCode::NxDomain) {
        auto authority = inspect_negative_authority(message, qclass);
        if (!authority) {
            return std::unexpected(authority.error());
        }
        if (authority->has_soa_ttl) {
            const DnsCacheKey key{qname, dns_cache_hash(qname)};
            err = cache_->upsert_nxdomain(
                    key, ttl_deadline(now, authority->soa_ttl, options_.min_negative_ttl, options_.max_negative_ttl));
            if (err != common::IoErr::None) {
                return std::unexpected(err);
            }
            outcome.action = PendingAction::RetryFromCache;
            return outcome;
        }
        outcome.status = ResolveStatus::NxDomain;
        return outcome;
    }

    std::array<char, kMaxDnsNameLen + 1> first_chain_name_buf{};
    std::array<char, kMaxDnsNameLen + 1> second_chain_name_buf{};
    std::string_view chain_owner = qname;
    bool use_first_chain_buf = true;
    bool has_relevant_answer = false;
    std::uint16_t response_cname_hops = 0;

    for (;;) {
        char *target_storage = use_first_chain_buf ? first_chain_name_buf.data() : second_chain_name_buf.data();
        const std::size_t target_storage_cap =
                use_first_chain_buf ? first_chain_name_buf.size() : second_chain_name_buf.size();
        AnswerSetView answer_set;
        err = inspect_answer_set(message, chain_owner, qtype, qclass, target_storage, target_storage_cap, answer_set);
        if (err != common::IoErr::None) {
            return std::unexpected(err);
        }
        if (answer_set.kind == AnswerSetKind::Address) {
            has_relevant_answer = true;
            break;
        }
        if (answer_set.kind != AnswerSetKind::Cname) {
            break;
        }
        if (response_cname_hops >= options_.max_cname_hops) {
            return std::unexpected(common::IoErr::Invalid);
        }
        ++response_cname_hops;
        has_relevant_answer = true;
        chain_owner = answer_set.cname_target;
        use_first_chain_buf = !use_first_chain_buf;
    }

    if (!has_relevant_answer) {
        if (message.answers.count != 0) {
            return std::unexpected(common::IoErr::Invalid);
        }
        auto authority = inspect_negative_authority(message, qclass);
        if (!authority) {
            return std::unexpected(authority.error());
        }
        if (authority->has_soa_ttl) {
            const DnsCacheKey key{qname, dns_cache_hash(qname)};
            const net::IpFamily family =
                    qtype == static_cast<std::uint16_t>(RecordType::A) ? net::IpFamily::V4 : net::IpFamily::V6;
            err = cache_->upsert_address_set(
                    key, family, nullptr, 0,
                    ttl_deadline(now, authority->soa_ttl, options_.min_negative_ttl, options_.max_negative_ttl));
            if (err != common::IoErr::None) {
                return std::unexpected(err);
            }
            outcome.action = PendingAction::RetryFromCache;
            return outcome;
        }
        outcome.status = authority->saw_ns ? ResolveStatus::ServerFailure : ResolveStatus::NoData;
        return outcome;
    }

    std::array<net::IpAddress, kDnsMaxAddressesPerFamily> temp_records{};

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
            return std::unexpected(err);
        }

        if (answer_set.kind == AnswerSetKind::Cname) {
            if (applied_cname_hops >= response_cname_hops) {
                return std::unexpected(common::IoErr::Invalid);
            }
            const DnsCacheKey key{chain_owner, dns_cache_hash(chain_owner)};
            err = cache_->upsert_cname(
                    key, answer_set.cname_target,
                    ttl_deadline(now, answer_set.ttl, options_.min_positive_ttl, options_.max_positive_ttl));
            if (err != common::IoErr::None) {
                return std::unexpected(err);
            }
            ++applied_cname_hops;
            chain_owner = answer_set.cname_target;
            use_first_chain_buf = !use_first_chain_buf;
            continue;
        }

        if (answer_set.kind == AnswerSetKind::Address) {
            std::array<char, kMaxDnsNameLen + 1> record_name_storage{};
            std::array<char, kMaxDnsNameLen + 1> owner_buf{};
            std::uint16_t count = 0;
            std::uint16_t seen_count = 0;
            MessageParser::RecordCursor cursor = MessageParser::cursor(message.answers);
            for (;;) {
                MessageParser::ResourceRecord record;
                auto next = MessageParser::next_record(message, cursor, record_name_storage.data(),
                                                       record_name_storage.size(), record);
                if (!next) {
                    return std::unexpected(next.error());
                }
                if (!*next) {
                    break;
                }
                if (record.type != qtype || record.dns_class != qclass) {
                    continue;
                }
                std::string_view normalized_owner;
                err = normalize_name(record.name, owner_buf.data(), owner_buf.size(), normalized_owner);
                if (err != common::IoErr::None) {
                    return std::unexpected(err);
                }
                if (normalized_owner != chain_owner) {
                    continue;
                }

                net::IpAddress address;
                const bool parsed = qtype == static_cast<std::uint16_t>(RecordType::A) ? parse_ipv4(record, address)
                                                                                       : parse_ipv6(record, address);
                if (!parsed) {
                    return std::unexpected(common::IoErr::Invalid);
                }
                ++seen_count;
                if (count < temp_records.size()) {
                    temp_records[count++] = address;
                }
            }
            if (seen_count != answer_set.address_count) {
                return std::unexpected(common::IoErr::Invalid);
            }

            const auto expire_at =
                    ttl_deadline(now, answer_set.ttl, options_.min_positive_ttl, options_.max_positive_ttl);
            const DnsCacheKey key{chain_owner, dns_cache_hash(chain_owner)};
            const net::IpFamily family =
                    qtype == static_cast<std::uint16_t>(RecordType::A) ? net::IpFamily::V4 : net::IpFamily::V6;
            err = cache_->upsert_address_set(key, family, temp_records.data(), count, expire_at);
            if (err != common::IoErr::None) {
                return std::unexpected(err);
            }
        }
        break;
    }

    outcome.action = PendingAction::RetryFromCache;
    return outcome;
}

} // namespace fiber::dns
