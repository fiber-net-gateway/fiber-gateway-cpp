#ifndef FIBER_QUIC_QUIC_UDP_ENDPOINT_H
#define FIBER_QUIC_QUIC_UDP_ENDPOINT_H

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "../async/Task.h"
#include "../common/IntrusiveList.h"
#include "../common/IntrusiveRbTree.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../common/mem/IoBufChain.h"
#include "../event/EventLoop.h"
#include "../net/SocketAddress.h"
#include "../net/UdpSocket.h"
#include "QuicConnection.h"
#include "QuicPacketProcessor.h"
#include "QuicSendScheduler.h"
#include "QuicToken.h"

namespace fiber::net {
class TlsServerContext;
} // namespace fiber::net

namespace fiber::quic {

inline constexpr std::size_t kQuicUdpDefaultReadBufferSize = 65536;
inline constexpr std::size_t kQuicUdpDefaultPlaintextBufferSize = 65536;
inline constexpr std::size_t kQuicUdpDefaultMaxRecvDatagramsPerWakeup = 64;
inline constexpr std::size_t kQuicUdpDefaultMaxRecvBytesPerWakeup = 256 * 1024;
inline constexpr std::size_t kQuicStatelessResetSecretLength = 32;
// Stateless reset packet sizing, matching nginx (ngx_event_quic_output.c):
// NGX_QUIC_MIN_PKT_LEN (41 = 21 + 20-byte server CID) is the smallest short
// header that may trigger a reset; NGX_QUIC_MIN_SR_PACKET (43) / MAX (1200) bound
// the generated reset's length.
inline constexpr std::size_t kQuicStatelessResetMinTriggerSize = 41;
inline constexpr std::size_t kQuicStatelessResetMinPacket = 43;
inline constexpr std::size_t kQuicStatelessResetMaxPacket = 1200;
// Per-endpoint stateless-reset rate-limit capacity: resets permitted per second.
inline constexpr std::size_t kQuicStatelessResetRateLimitCapacity = 8;
inline constexpr std::size_t kQuicStatelessPeerBucketCount = 256;

struct QuicUdpReceiveResult {
    QuicConnection *connection = nullptr;
    QuicPacketProcessResult packet{};
    bool created = false;
};

class QuicUdpEndpoint : public common::NonCopyable, public common::NonMovable {
public:
    struct StatelessResponseLimit {
        // Zero disables the corresponding endpoint-wide or per-peer ceiling.
        std::uint32_t endpoint_per_second = 0;
        std::uint16_t peer_per_second = 0;
    };

    struct StatelessResponseLimits {
        StatelessResponseLimit stateless_reset{kQuicStatelessResetRateLimitCapacity, 3};
        StatelessResponseLimit version_negotiation{256, 4};
        StatelessResponseLimit retry{4096, 8};
        StatelessResponseLimit invalid_token_close{128, 2};
    };

    struct Options {
        net::SocketAddress bind_addr{};
        std::size_t max_connections = 1024;
        net::TlsServerContext *tls_context = nullptr;
        net::UdpBindOptions udp{};
        QuicSendScheduler::Options send{};
        QuicTransportSettings transport{};
        std::chrono::milliseconds keepalive_interval{0};
        QuicRecvFlowControlSettings recv_flow{};
        std::chrono::milliseconds max_ack_delay{25};
        std::uint64_t ack_delay_exponent = 3;
        std::size_t max_recv_datagrams_per_wakeup = kQuicUdpDefaultMaxRecvDatagramsPerWakeup;
        std::size_t max_recv_bytes_per_wakeup = kQuicUdpDefaultMaxRecvBytesPerWakeup;
        std::size_t retained_storage_limit = kQuicDefaultEndpointRetainedStorageLimit;
        bool retry = false;
        bool issue_new_token = false;
        bool address_validation_key_set = false;
        std::array<std::uint8_t, kQuicAddressValidationKeyLength> address_validation_key{};
        bool stateless_reset_secret_set = false;
        std::array<std::uint8_t, kQuicStatelessResetSecretLength> stateless_reset_secret{};
        StatelessResponseLimits stateless_response_limits{};
        std::chrono::seconds retry_token_lifetime{3};
        std::chrono::seconds new_token_lifetime{600};
        void *connection_owner = nullptr;
        // Required. The endpoint never allocates QuicConnection itself; this
        // callback must return an owning lease whose destroy callback releases
        // the concrete connection storage when ref_count reaches zero.
        QuicConnection::Lease (*create_connection)(void *owner,
                                                   const QuicConnection::Options &options) noexcept = nullptr;
        bool enable_early_data = false;
    };

    QuicUdpEndpoint() noexcept;
    ~QuicUdpEndpoint();

    [[nodiscard]] common::IoResult<void> init(event::EventLoop &loop, const Options &options) noexcept;
    // Starts callback-driven I/O and must run on the endpoint's event loop.
    [[nodiscard]] common::IoResult<void> start() noexcept;
    void close() noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool running() const noexcept { return started_ && !closing_; }
    [[nodiscard]] const net::SocketAddress &local_addr() const noexcept;
    [[nodiscard]] std::size_t active_connection_count() const noexcept { return active_connection_count_; }
    [[nodiscard]] std::size_t dropped_datagram_count() const noexcept { return dropped_datagram_count_; }
    [[nodiscard]] std::size_t rejected_connection_count() const noexcept { return rejected_connection_count_; }
    [[nodiscard]] std::size_t retained_recv_storage_capacity() const noexcept {
        return recv_storage_budget_.retained_capacity();
    }
    [[nodiscard]] std::size_t retained_recv_storage_high_water() const noexcept {
        return recv_storage_budget_.high_water();
    }
    [[nodiscard]] std::size_t retained_recv_storage_rejected_count() const noexcept {
        return recv_storage_budget_.rejected_count();
    }
    [[nodiscard]] std::size_t rate_limited_stateless_response_count() const noexcept {
        return rate_limited_stateless_response_count_;
    }
    [[nodiscard]] mem::IoBufNodePool &recv_extent_pool() noexcept { return loop_->io_buf_node_pool(); }

    [[nodiscard]] QuicConnection *find_connection(const QuicConnectionId &dcid) noexcept;
    [[nodiscard]] const QuicConnection *find_connection(const QuicConnectionId &dcid) const noexcept;
    [[nodiscard]] common::IoResult<void> remove_connection(const QuicConnectionId &dcid) noexcept;
    void schedule_send(QuicConnection &connection) noexcept;

    // Manual one-shot receive for focused callers and tests. It is unavailable
    // after start() because readiness callbacks and awaiters share the read slot.
    [[nodiscard]] async::Task<common::IoResult<QuicUdpReceiveResult>> recv_once() noexcept;

private:
    friend class QuicSendScheduler;
    friend class QuicConnection;

    struct QuicConnectionDcidLess {
        [[nodiscard]] bool operator()(const QuicConnectionIdIndex *left,
                                      const QuicConnectionIdIndex *right) const noexcept;
    };

    using DcidTree = common::IntrusiveRbTree<QuicConnectionIdIndex, offsetof(QuicConnectionIdIndex, cid_hook),
                                             QuicConnectionDcidLess>;
    using ConnectionList =
            common::IntrusiveList<QuicConnection::EndpointIndex, offsetof(QuicConnection::EndpointIndex, link)>;

    enum class QuicInitialValidationAction : std::uint8_t {
        Accept,
        SendRetry,
        SendInvalidTokenClose,
    };

    enum class QuicStatelessResponseKind : std::uint8_t {
        StatelessReset,
        VersionNegotiation,
        Retry,
        InvalidTokenClose,
        Count,
    };

    static constexpr std::size_t kStatelessResponseKindCount =
            static_cast<std::size_t>(QuicStatelessResponseKind::Count);

    struct QuicStatelessPeerBucket {
        std::uint64_t peer_hash = 0;
        std::uint64_t epoch = 0;
        std::array<std::uint16_t, kStatelessResponseKindCount> used{};
        bool occupied = false;
    };

    struct QuicStatelessRateState {
        std::uint64_t global_epoch = 0;
        std::array<std::uint32_t, kStatelessResponseKindCount> global_used{};
        std::array<QuicStatelessPeerBucket, kQuicStatelessPeerBucketCount> peers{};
        bool global_initialized = false;
    };

    struct QuicInitialValidation {
        QuicConnectionId original_destination_connection_id{};
        QuicConnectionId retry_source_connection_id{};
        const char *close_reason = nullptr;
        QuicInitialValidationAction action = QuicInitialValidationAction::Accept;
        bool address_validated = false;
        bool retried = false;
    };

    struct ReceivePumpResult {
        common::IoErr error = common::IoErr::None;
        std::size_t datagrams_received = 0;
        std::size_t bytes_received = 0;
        bool needs_reschedule = false;
    };

    [[nodiscard]] static std::uint64_t hash_connection_id(const QuicConnectionId &id) noexcept;
    [[nodiscard]] static std::uint64_t hash_stateless_peer(const net::SocketAddress &peer) noexcept;
    [[nodiscard]] static int compare_connection_id(const QuicConnectionId &left,
                                                   const QuicConnectionId &right) noexcept;
    [[nodiscard]] static int compare_dcid_key(std::uint64_t left_hash, const QuicConnectionId &left,
                                              std::uint64_t right_hash, const QuicConnectionId &right) noexcept;
    [[nodiscard]] static QuicConnectionIdIndex *index_from_dcid_hook(common::IntrusiveRbTreeHook *hook) noexcept;
    [[nodiscard]] static const QuicConnectionIdIndex *
    index_from_dcid_hook(const common::IntrusiveRbTreeHook *hook) noexcept;

    [[nodiscard]] QuicConnection *find_connection(const QuicConnectionId &dcid, std::uint64_t hash) noexcept;
    [[nodiscard]] const QuicConnection *find_connection(const QuicConnectionId &dcid,
                                                        std::uint64_t hash) const noexcept;
    void detach_connection(QuicConnection &connection) noexcept;
    void force_detach_connection(QuicConnection &connection) noexcept;
    [[nodiscard]] common::IoResult<QuicConnectionId> generate_connection_id() noexcept;
    [[nodiscard]] common::IoResult<QuicConnectionId> generate_unique_connection_id() noexcept;
    [[nodiscard]] common::IoResult<void> register_connection_id(QuicConnection &connection,
                                                                QuicConnectionIdIndex &index,
                                                                const QuicConnectionId &cid) noexcept;
    void unregister_connection_id(QuicConnectionIdIndex &index) noexcept;
    [[nodiscard]] common::IoResult<void>
    create_stateless_reset_token(const QuicConnectionId &cid,
                                 std::uint8_t out[kStatelessResetTokenLength]) const noexcept;
    [[nodiscard]] common::IoResult<bool> fill_local_connection_ids(QuicConnection &connection) noexcept;
    [[nodiscard]] common::IoResult<bool>
    retire_local_connection_id_and_resend(QuicConnection &connection, std::uint64_t sequence_number,
                                          const QuicConnectionId &packet_dcid) noexcept;
    [[nodiscard]] common::IoResult<QuicInitialValidation>
    validate_initial_address(const QuicPacketHeader &packet, const QuicReceivedDatagram &datagram) noexcept;
    [[nodiscard]] common::IoResult<void> send_direct_datagram(const std::uint8_t *data, std::size_t len,
                                                              const QuicReceivedDatagram &datagram) noexcept;
    [[nodiscard]] bool allow_stateless_response(QuicStatelessResponseKind kind, const net::SocketAddress &peer,
                                                std::chrono::steady_clock::time_point now) noexcept;
    [[nodiscard]] const StatelessResponseLimit &stateless_response_limit(QuicStatelessResponseKind kind) const noexcept;
    [[nodiscard]] common::IoResult<void> send_stateless_reset(const QuicPacketHeader &packet, std::uint8_t *out,
                                                              std::size_t out_cap,
                                                              const QuicReceivedDatagram &datagram) noexcept;
    [[nodiscard]] common::IoResult<void> queue_new_token(QuicConnection &connection, QuicPath &path) noexcept;
    [[nodiscard]] common::IoResult<QuicConnection *>
    create_connection(const QuicPacketHeader &packet, const QuicReceivedDatagram &datagram,
                      const QuicInitialValidation &validation) noexcept;
    [[nodiscard]] common::IoResult<QuicUdpReceiveResult>
    process_datagram(net::UdpPacketRecvResult recv, std::chrono::steady_clock::time_point now) noexcept;
    [[nodiscard]] ReceivePumpResult pump_receive() noexcept;
    [[nodiscard]] common::IoErr sync_socket_callbacks() noexcept;
    void clear_socket_callbacks() noexcept;
    void schedule_io_pump() noexcept;
    void drive_io() noexcept;
    void handle_socket_ready(event::IoEvent event, common::IoErr err) noexcept;
    static void on_socket_read_ready(void *ctx, common::IoErr err) noexcept;
    static void on_socket_write_ready(void *ctx, common::IoErr err) noexcept;
    static void on_io_pump(QuicUdpEndpoint *endpoint) noexcept;
    [[nodiscard]] common::IoResult<QuicBuildSendResult>
    build_send_datagram(QuicConnection &connection, QuicSendDatagram &datagram,
                        QuicBuildMode mode = QuicBuildMode::Normal) noexcept;
    [[nodiscard]] common::IoResult<QuicBuildSendResult>
    build_path_control_datagram(QuicConnection &connection, QuicSendDatagram &datagram) noexcept;
    [[nodiscard]] static common::IoResult<QuicStreamFrameEncodeStatus>
    encode_stream_frame_into_payload(QuicConnection &connection, QuicOutputFrame &frame, std::uint8_t *dst,
                                     std::size_t available) noexcept;
    void commit_send_datagram(QuicConnection &connection, const QuicSendDatagram &datagram) noexcept;
    void rollback_send_datagram(QuicConnection &connection, const QuicSendDatagram &datagram) noexcept;
    [[nodiscard]] bool connection_has_send_work(const QuicConnection &connection) const noexcept;
    void handle_receive_result(QuicConnection &connection, const QuicPacketProcessResult &result) noexcept;
    [[nodiscard]] bool should_delay_ack(const QuicPacketNumberSpace &space, QuicTime now) const noexcept;

    Options options_{};
    event::EventLoop *loop_ = nullptr;
    std::unique_ptr<net::UdpSocket> socket_{};
    std::unique_ptr<std::uint8_t[]> read_buffer_{};
    std::unique_ptr<std::uint8_t[]> send_plaintext_buffer_{};
    std::unique_ptr<std::uint8_t[]> send_buffer_{};
    mem::IoBufStorageBudget recv_storage_budget_{};
    QuicCryptoBlockPool crypto_block_pool_{};
    QuicOutputFramePool output_frame_pool_{};
    QuicSendScheduler send_scheduler_{};
    DcidTree dcid_tree_{};
    ConnectionList connections_{};
    std::size_t active_connection_count_ = 0;
    std::size_t dropped_datagram_count_ = 0;
    std::size_t rejected_connection_count_ = 0;
    std::size_t rate_limited_stateless_response_count_ = 0;
    // Fixed-memory per-kind limiter. The endpoint-wide ceiling remains
    // effective when an attacker rotates spoofed source addresses; the
    // direct-mapped peer table adds a tighter per-IP ceiling without allowing
    // network traffic to drive dynamic allocation.
    QuicStatelessRateState stateless_rate_{};
    event::EventLoop::DeferEntry io_pump_entry_{};
    bool initialized_ = false;
    bool started_ = false;
    bool closing_ = false;
    bool read_callback_registered_ = false;
    bool write_callback_registered_ = false;
    bool read_ready_ = false;
    bool write_ready_ = false;
    bool write_blocked_ = false;
    bool io_pump_running_ = false;
    bool io_pump_again_ = false;
    bool prefer_write_ = false;
};

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_UDP_ENDPOINT_H
