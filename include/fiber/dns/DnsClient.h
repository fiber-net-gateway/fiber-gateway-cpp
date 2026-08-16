#ifndef FIBER_DNS_DNS_CLIENT_H
#define FIBER_DNS_DNS_CLIENT_H

#include <array>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../event/EventLoop.h"
#include "../net/SocketAddress.h"
#include "DnsProtocol.h"
#include "DnsResolverConfig.h"
#include "detail/DnsUdpSendQueue.h"

namespace fiber::net {
class UdpSocket;
}

namespace fiber::dns {

class DnsClient : public common::NonCopyable, public common::NonMovable {
public:
    enum class ResponseDisposition : std::uint8_t {
        Accept,
        RetryServer,
    };

    struct ResponseValidator {
        using ValidateFn = ResponseDisposition (*)(void *context, const std::uint8_t *packet,
                                                   std::size_t packet_len) noexcept;

        void *context = nullptr;
        ValidateFn validate = nullptr;
    };

    struct Options {
        // Required ordered, fixed-capacity upstream list. To migrate a former single-server
        // configuration, replace `options.server = address` with
        // `options.nameservers.add(address)` and handle the returned capacity error.
        DnsNameserverList nameservers{};
        // A specific bind address supports nameservers from one IP family. Leave this
        // unspecified to let a mixed IPv4/IPv6 list create one UDP transport per family.
        net::SocketAddress bind_addr{};
        // Per-server UDP deadline, covering send-queue wait, send, and response wait. A TCP
        // fallback gets one deadline of the same duration for connect/write/read together.
        std::chrono::milliseconds timeout{2000};
        // Number of complete passes over the ordered server list. Thus a query performs at
        // most attempts * nameservers.size() UDP attempts before returning.
        std::uint8_t attempts = 2;
        bool enable_tcp_fallback = true;
        bool enable_0x20 = true;
        std::uint16_t max_inflight = 128;
        std::uint16_t max_udp_packet_size = 1232;
        QueryOptions query_options{};
        // false starts every query at nameservers[0]; true advances the initial server once
        // per query. Failover always proceeds in list order and wraps at the end.
        bool rotate_nameservers = false;
        // FORMERR, SERVFAIL, NOTIMP, and REFUSED proceed to the next scheduled server. If all
        // scheduled attempts return one of these responses, the last response is returned.
        bool retry_server_errors = true;
    };

    DnsClient() noexcept;
    ~DnsClient();

    // Queries and lifecycle teardown run on the bound loop after initialization. close cancels
    // queued UDP sends, response waits, and in-progress TCP fallback before release.
    [[nodiscard]] bool init(event::EventLoop &loop, Options options) noexcept;
    void close() noexcept;
    void release() noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] event::EventLoop &loop() const noexcept;
    [[nodiscard]] std::span<const net::SocketAddress> servers() const noexcept { return options_.nameservers.view(); }

    async::Task<common::IoResult<std::size_t>> query_raw(const QuestionSpec &question, std::uint8_t *dst,
                                                         std::size_t cap) noexcept;
    // A validator may classify an otherwise matching, non-truncated response as unusable.
    // RetryServer proceeds to the next scheduled server without copying that UDP packet to dst.
    // TCP fallback is attempted against the same server that returned the truncated UDP reply.
    async::Task<common::IoResult<std::size_t>> query_raw(const QuestionSpec &question, std::uint8_t *dst,
                                                         std::size_t cap, ResponseValidator validator) noexcept;

private:
    static constexpr std::uint16_t kInvalidSlot = 0xffffU;
    static constexpr std::uint16_t kInvalidIdMapping = 0xffffU;
    using InflightCancelFn = void (*)(void *) noexcept;

    struct InflightSlot {
        bool active = false;
        bool completed = false;
        bool need_tcp_fallback = false;
        bool retry_server = false;
        std::uint8_t server_index = 0;
        std::uint16_t id = 0;
        std::uint16_t next_free = kInvalidSlot;
        std::coroutine_handle<> waiter{};
        std::uint8_t *request_buf = nullptr;
        std::size_t request_size = 0;
        std::uint8_t *response_dst = nullptr;
        std::size_t response_cap = 0;
        std::size_t response_size = 0;
        common::IoErr completion_err = common::IoErr::None;
        ResponseValidator validator{};
        void *cancel_context = nullptr;
        InflightCancelFn cancel = nullptr;
    };

    struct UdpTransport {
        UdpTransport() noexcept;
        ~UdpTransport();

        DnsClient *owner = nullptr;
        net::IpFamily family = net::IpFamily::V4;
        std::unique_ptr<net::UdpSocket> socket{};
        detail::DnsUdpSendQueue send_queue{};
        bool active = false;
        bool read_callback_registered = false;
    };

    // Completing a slot resumes upper-layer code synchronously. That code may close,
    // release, or destroy this client before the read callback regains control.
    class UdpReadDispatchGuard {
    public:
        explicit UdpReadDispatchGuard(DnsClient &owner) noexcept;
        ~UdpReadDispatchGuard() noexcept;

        UdpReadDispatchGuard(const UdpReadDispatchGuard &) = delete;
        UdpReadDispatchGuard &operator=(const UdpReadDispatchGuard &) = delete;
        UdpReadDispatchGuard(UdpReadDispatchGuard &&) = delete;
        UdpReadDispatchGuard &operator=(UdpReadDispatchGuard &&) = delete;

        [[nodiscard]] bool invalidated() const noexcept { return invalidated_; }

    private:
        DnsClient *owner_ = nullptr;
        bool invalidated_ = false;
    };

    class ResponseAwaiter {
    public:
        ResponseAwaiter(DnsClient &client, std::uint16_t slot_index) noexcept;
        ~ResponseAwaiter();

        bool await_ready() noexcept;
        bool await_suspend(std::coroutine_handle<> handle) noexcept;
        common::IoResult<std::size_t> await_resume() noexcept;

    private:
        DnsClient *client_ = nullptr;
        std::uint16_t slot_index_ = kInvalidSlot;
        std::coroutine_handle<> handle_{};
        bool armed_ = false;
    };

    async::Task<common::IoResult<std::size_t>> query_tcp(std::uint16_t slot_index, std::uint8_t server_index) noexcept;

    [[nodiscard]] bool init_storage() noexcept;
    [[nodiscard]] bool init_udp_transport(net::IpFamily family, const net::SocketAddress &bind_addr) noexcept;
    [[nodiscard]] common::IoErr ensure_udp_read_callbacks() noexcept;
    void drain_udp_reads(UdpTransport &transport) noexcept;
    void invalidate_udp_read_dispatch() noexcept;
    static void on_udp_readable(void *ctx, common::IoErr err) noexcept;
    [[nodiscard]] common::IoResult<std::size_t> prepare_request(InflightSlot &slot,
                                                                const QuestionSpec &question) noexcept;
    void reset_state() noexcept;
    void cancel_all_inflight(common::IoErr err) noexcept;

    [[nodiscard]] std::uint16_t allocate_slot() noexcept;
    void release_slot(std::uint16_t slot_index) noexcept;
    [[nodiscard]] common::IoResult<std::uint16_t> allocate_query_id(std::uint16_t start, std::uint16_t stride) noexcept;
    void clear_query_id(std::uint16_t id, std::uint16_t slot_index) noexcept;

    [[nodiscard]] bool response_ready(std::uint16_t slot_index) const noexcept;
    [[nodiscard]] bool arm_waiter(std::uint16_t slot_index, std::coroutine_handle<> handle) noexcept;
    void cancel_waiter(std::uint16_t slot_index, std::coroutine_handle<> handle) noexcept;
    [[nodiscard]] common::IoResult<std::size_t> wait_result(std::uint16_t slot_index) noexcept;
    void arm_inflight_cancel(std::uint16_t slot_index, void *context, InflightCancelFn cancel) noexcept;
    void disarm_inflight_cancel(std::uint16_t slot_index, void *context) noexcept;

    void complete_slot(std::uint16_t slot_index, common::IoErr err, std::size_t response_size, bool need_tcp_fallback,
                       bool retry_server = false) noexcept;
    void handle_udp_packet(const std::uint8_t *packet, std::size_t packet_len, const net::SocketAddress &peer) noexcept;
    [[nodiscard]] ResponseDisposition validate_response(const InflightSlot &slot, const std::uint8_t *packet,
                                                        std::size_t packet_len) const noexcept;
    [[nodiscard]] bool should_retry_server_error(const std::uint8_t *packet, std::size_t packet_len) const noexcept;
    [[nodiscard]] bool has_open_udp_transport() const noexcept;
    [[nodiscard]] UdpTransport &udp_transport(net::IpFamily family) noexcept;
    [[nodiscard]] const UdpTransport &udp_transport(net::IpFamily family) const noexcept;

    Options options_{};
    event::EventLoop *loop_ = nullptr;
    std::array<UdpTransport, 2> udp_transports_{};
    std::unique_ptr<InflightSlot[]> slots_{};
    std::unique_ptr<std::uint16_t[]> id_to_slot_{};
    std::unique_ptr<std::uint8_t[]> request_buffers_{};
    std::unique_ptr<std::uint8_t[]> recv_buffer_{};
    bool *udp_read_dispatch_invalidated_observer_ = nullptr;
    std::uint16_t free_head_ = kInvalidSlot;
    std::uint8_t next_server_ = 0;
    bool closing_ = false;
};

} // namespace fiber::dns

#endif // FIBER_DNS_DNS_CLIENT_H
