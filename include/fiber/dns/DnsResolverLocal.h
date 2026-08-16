#ifndef FIBER_DNS_DNS_RESOLVER_LOCAL_H
#define FIBER_DNS_DNS_RESOLVER_LOCAL_H

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../event/EventLoop.h"
#include "../net/IpAddress.h"
#include "DnsAddress.h"
#include "DnsCache2.h"
#include "DnsClient.h"
#include "DnsMessage.h"

namespace fiber::dns {

enum class ResolveStatus : std::uint8_t {
    Success,
    NxDomain,
    NoData,
    FormatError,
    ServerFailure,
    NotImplemented,
    Refused,
};

class ResolveResult {
public:
    ResolveResult() noexcept = default;

    void clear() noexcept;

    [[nodiscard]] std::string_view canonical_name() const noexcept {
        return std::string_view(canonical_name_, canonical_name_len_);
    }
    [[nodiscard]] const DnsAddressSet &address_set() const noexcept { return addresses_; }
    [[nodiscard]] const net::IpAddress *records() const noexcept { return addresses_.records; }
    [[nodiscard]] std::uint16_t record_count() const noexcept { return addresses_.count; }
    [[nodiscard]] std::chrono::steady_clock::time_point expire_at() const noexcept { return expire_at_; }

private:
    friend class DnsResolverLocal;

    [[nodiscard]] common::IoErr assign_positive(std::string_view canonical_name, const net::IpAddress *records,
                                                std::uint16_t count,
                                                std::chrono::steady_clock::time_point expire_at) noexcept;
    [[nodiscard]] common::IoErr assign_canonical(std::string_view canonical_name) noexcept;

    DnsAddressSet addresses_{};
    char canonical_name_[256]{};
    std::uint16_t canonical_name_len_ = 0;
    std::chrono::steady_clock::time_point expire_at_{};
};

class DnsResolverLocal : public common::NonCopyable, public common::NonMovable {
public:
    struct Options {
        std::uint16_t max_cname_hops = 8;
        std::uint16_t max_pending = 64;
        std::uint16_t max_packet_size = 4096;
        std::chrono::seconds min_positive_ttl{1};
        std::chrono::seconds max_positive_ttl{300};
        std::chrono::seconds min_negative_ttl{1};
        std::chrono::seconds max_negative_ttl{60};
        MessageParser::Options parser_options{};
    };

    DnsResolverLocal() noexcept = default;
    ~DnsResolverLocal();

    [[nodiscard]] bool init(event::EventLoop &loop, SharedDnsCache2 &cache,
                            DnsClient::Options client_options) noexcept {
        return init(loop, cache, client_options, Options{});
    }
    [[nodiscard]] bool init(event::EventLoop &loop, SharedDnsCache2 &cache, DnsClient::Options client_options,
                            Options options) noexcept;
    void close() noexcept;
    void release() noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] event::EventLoop &loop() const noexcept;

    [[nodiscard]] async::Task<common::IoResult<ResolveStatus>> resolve(const QuestionSpec &question,
                                                                       ResolveResult &out) noexcept;

private:
    static constexpr std::uint16_t kInvalidPending = 0xffffU;
    static constexpr std::size_t kMaxNormalizedNameLen = 255;

    enum class PendingAction : std::uint8_t {
        ReturnStatus,
        RetryFromCache,
    };

    struct PendingOutcome {
        common::IoErr err = common::IoErr::None;
        PendingAction action = PendingAction::ReturnStatus;
        ResolveStatus status = ResolveStatus::ServerFailure;
    };

    struct PendingWaiter {
        PendingWaiter *next = nullptr;
        std::coroutine_handle<> handle{};
        PendingOutcome outcome{};
    };

    struct PendingEntry {
        bool active = false;
        std::uint16_t qtype = 0;
        std::uint16_t qclass = 0;
        std::uint16_t next_free = kInvalidPending;
        std::uint16_t name_len = 0;
        char name[kMaxNormalizedNameLen + 1]{};
        std::uint8_t *packet_buf = nullptr;
        PendingWaiter *waiters = nullptr;
    };

    class PendingAwaiter {
    public:
        PendingAwaiter(DnsResolverLocal &resolver, std::uint16_t pending_index) noexcept;
        ~PendingAwaiter();

        bool await_ready() const noexcept { return false; }
        bool await_suspend(std::coroutine_handle<> handle) noexcept;
        PendingOutcome await_resume() noexcept;

    private:
        DnsResolverLocal *resolver_ = nullptr;
        std::uint16_t pending_index_ = kInvalidPending;
        PendingWaiter waiter_{};
        bool queued_ = false;
    };

    [[nodiscard]] bool init_pending_storage() noexcept;
    void release_pending_storage() noexcept;
    void reset_state() noexcept;
    void cancel_all_pending(common::IoErr err) noexcept;

    [[nodiscard]] std::uint16_t find_pending(std::string_view qname, std::uint16_t qtype,
                                             std::uint16_t qclass) const noexcept;
    [[nodiscard]] std::uint16_t allocate_pending(std::string_view qname, std::uint16_t qtype,
                                                 std::uint16_t qclass) noexcept;
    void release_pending(std::uint16_t index) noexcept;
    [[nodiscard]] bool enqueue_waiter(std::uint16_t index, PendingWaiter *waiter) noexcept;
    void cancel_waiter(std::uint16_t index, PendingWaiter *waiter) noexcept;
    void finish_pending(std::uint16_t index, PendingOutcome outcome) noexcept;

    [[nodiscard]] async::Task<common::IoResult<PendingOutcome>>
    query_upstream(std::string_view qname, std::uint16_t qtype, std::uint16_t qclass, PendingEntry &pending) noexcept;

    [[nodiscard]] static DnsClient::ResponseDisposition
    validate_upstream_response(void *context, const std::uint8_t *packet, std::size_t packet_len) noexcept;

    [[nodiscard]] common::IoResult<PendingOutcome> handle_response(std::string_view qname, std::uint16_t qtype,
                                                                   std::uint16_t qclass, const std::uint8_t *packet,
                                                                   std::size_t packet_len) noexcept;

    Options options_{};
    event::EventLoop *loop_ = nullptr;
    SharedDnsCache2 *cache_ = nullptr;
    DnsClient client_{};
    MessageParser parser_{};
    std::unique_ptr<PendingEntry[]> pending_{};
    std::unique_ptr<std::uint8_t[]> packet_storage_{};
    std::uint16_t free_head_ = kInvalidPending;
    bool closing_ = false;
};

} // namespace fiber::dns

#endif // FIBER_DNS_DNS_RESOLVER_LOCAL_H
