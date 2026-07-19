#ifndef FIBER_QUIC_QUIC_CONNECTION_H
#define FIBER_QUIC_QUIC_CONNECTION_H

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

#include <openssl/aead.h>
#include <openssl/aes.h>

#include "../common/IntrusiveList.h"
#include "../common/IntrusiveRbTree.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../common/mem/IoBufChain.h"
#include "../event/EventLoop.h"
#include "../net/SocketAddress.h"
#include "QuicCongestion.h"
#include "QuicConnectionId.h"
#include "QuicFrame.h"
#include "QuicPacer.h"
#include "QuicPacketNumberSpace.h"
#include "QuicPath.h"
#include "QuicPathManager.h"
#include "QuicStream.h"
#include "QuicStreamTable.h"
#include "QuicTlsSession.h"

namespace fiber::quic {

struct QuicTransportParams;
class QuicSendScheduler;
class QuicUdpEndpoint;

inline constexpr std::size_t kQuicInitialSecretLength = 32;
inline constexpr std::size_t kQuicMaxSecretLength = 48;
inline constexpr std::size_t kQuicMaxKeyLength = 32;
inline constexpr std::size_t kQuicIvLength = 12;
inline constexpr std::size_t kQuicMaxHeaderProtectionKeyLength = 32;
inline constexpr std::size_t kQuicInitialKeyLength = 16;
inline constexpr std::size_t kQuicInitialIvLength = kQuicIvLength;
inline constexpr std::size_t kQuicInitialHeaderProtectionKeyLength = 16;
inline constexpr std::size_t kQuicHeaderProtectionSampleLength = 16;
inline constexpr std::size_t kQuicHeaderProtectionMaskLength = 5;
inline constexpr std::size_t kQuicMaxUdpPayloadSize = 65527;
inline constexpr std::size_t kQuicDefaultStreamBufferSize = 65536;
inline constexpr std::uint64_t kQuicDefaultConnRecvLimit = 2ULL * 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kQuicDefaultConnRecvLowWater = 10ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kQuicDefaultMaxBidirectionalStreams = 128;
inline constexpr std::uint64_t kQuicDefaultMaxUnidirectionalStreams = 128;
inline constexpr std::uint64_t kQuicDefaultInitialMaxData = kQuicDefaultConnRecvLimit;

enum class QuicConnectionRole : std::uint8_t {
    Client,
    Server,
};

enum class QuicConnectionState : std::uint8_t {
    Init,
    Handshaking,
    Established,
    GracefulClosing,
    Closing,
    Draining,
    Closed,
};

enum class QuicCloseSource : std::uint8_t {
    None,
    Local,
    PeerConnectionClose,
    StatelessReset,
    IdleTimeout,
};

enum class QuicCloseFrameKind : std::uint8_t {
    Transport,
    Application,
};

struct QuicCloseInfo {
    QuicCloseSource source = QuicCloseSource::None;
    QuicCloseFrameKind frame_kind = QuicCloseFrameKind::Transport;
    std::uint64_t error_code = 0;
    std::uint64_t frame_type = 0;
};

enum class QuicErrorCode : std::uint64_t {
    NoError = 0x00,
    InternalError = 0x01,
    ConnectionRefused = 0x02,
    FlowControlError = 0x03,
    StreamLimitError = 0x04,
    StreamStateError = 0x05,
    FinalSizeError = 0x06,
    FrameEncodingError = 0x07,
    TransportParameterError = 0x08,
    ConnectionIdLimitError = 0x09,
    ProtocolViolation = 0x0A,
    InvalidToken = 0x0B,
    ApplicationError = 0x0C,
    CryptoBufferExceeded = 0x0D,
    KeyUpdateError = 0x0E,
    AeadLimitReached = 0x0F,
    NoViablePath = 0x10,
};

// RFC 9000 §20.1: a TLS failure during the handshake is reported to the peer as
// a CRYPTO_ERROR transport error code = 0x0100 | (TLS alert value), spanning
// 0x0100–0x01FF. nginx models this as NGX_QUIC_ERR_CRYPTO(alert). Special
// alerts such as no_application_protocol (120) and missing_extension (109) are
// encoded correctly by the general 0x0100 | alert mapping — no special-casing.
inline constexpr std::uint64_t kQuicCryptoErrorBase = 0x0100u;

[[nodiscard]] constexpr std::uint64_t quic_crypto_error_code(std::uint8_t alert) noexcept {
    return kQuicCryptoErrorBase | static_cast<std::uint64_t>(alert);
}

struct QuicTransportSettings {
    std::chrono::milliseconds max_idle_timeout = std::chrono::seconds(30);
    std::size_t max_udp_payload_size = kQuicMaxUdpPayloadSize;
    std::uint64_t initial_max_data = kQuicDefaultInitialMaxData;
    std::uint64_t initial_max_stream_data_bidi_local = kQuicDefaultStreamBufferSize;
    std::uint64_t initial_max_stream_data_bidi_remote = kQuicDefaultStreamBufferSize;
    std::uint64_t initial_max_stream_data_uni = kQuicDefaultStreamBufferSize;
    std::uint64_t initial_max_streams_bidi = kQuicDefaultMaxBidirectionalStreams;
    std::uint64_t initial_max_streams_uni = kQuicDefaultMaxUnidirectionalStreams;
    std::uint64_t ack_delay_exponent = 3;
    std::chrono::milliseconds max_ack_delay{25};
    std::uint64_t active_connection_id_limit = 4;
    bool disable_active_migration = false;
};

struct QuicRecvFlowControlSettings {
    std::uint64_t conn_recv_limit = kQuicDefaultConnRecvLimit;
    std::uint64_t conn_recv_low_water = kQuicDefaultConnRecvLowWater;
    std::size_t stream_buffer_limit = kQuicDefaultStreamBufferSize;
    std::size_t stream_low_water = kQuicStreamRecvDefaultLowWater;
};

struct QuicPeerTransportState {
    QuicTransportSettings params{};
    bool received = false;
};

class QuicConnection;

enum class QuicCryptoSuite : std::uint8_t {
    InitialAes128GcmSha256,
    Aes128GcmSha256,
    Aes256GcmSha384,
    ChaCha20Poly1305Sha256,
};

struct QuicPacketProtectionKeys : public common::NonCopyable, public common::NonMovable {
    QuicPacketProtectionKeys() noexcept;
    ~QuicPacketProtectionKeys();

    void reset() noexcept;
    // POD-only field-by-field swap. Used by the key-update path to rotate
    // application_read/write ↔ next_application_read/write without moving the
    // owning struct (it is NonMovable to keep references stable).
    void swap(QuicPacketProtectionKeys &other) noexcept;

    QuicCryptoSuite suite = QuicCryptoSuite::InitialAes128GcmSha256;
    std::array<std::uint8_t, kQuicMaxSecretLength> secret{};
    std::array<std::uint8_t, kQuicMaxKeyLength> key{};
    std::array<std::uint8_t, kQuicIvLength> iv{};
    std::array<std::uint8_t, kQuicMaxHeaderProtectionKeyLength> hp{};
    std::size_t secret_len = 0;
    std::size_t key_len = 0;
    std::size_t iv_len = 0;
    std::size_t hp_len = 0;
    EVP_AEAD_CTX aead{};
    AES_KEY hp_key{};
    bool aead_initialized = false;
    bool hp_chacha20 = false;
    bool ready = false;
};

struct QuicCryptoState : public common::NonCopyable, public common::NonMovable {
    QuicCryptoState() noexcept = default;

    void reset() noexcept;

    QuicPacketProtectionKeys initial_read{};
    QuicPacketProtectionKeys initial_write{};
    QuicPacketProtectionKeys early_read{};
    QuicPacketProtectionKeys early_write{};
    QuicPacketProtectionKeys handshake_read{};
    QuicPacketProtectionKeys handshake_write{};
    QuicPacketProtectionKeys application_read{};
    QuicPacketProtectionKeys application_write{};
    // Pre-derived next-generation application keys (RFC 9001 §6.1, "tls13 quic ku").
    // Populated after the handshake is confirmed so the connection can immediately
    // respond to a peer-initiated key update.
    QuicPacketProtectionKeys next_application_read{};
    QuicPacketProtectionKeys next_application_write{};
    // Previous-generation application keys, retained for a grace period after a key
    // update to decrypt reordered packets that still carry the old key phase
    // (RFC 9001 §6.5).
    QuicPacketProtectionKeys previous_application_read{};
    bool initial_ready = false;
    bool next_application_keys_ready = false;
    bool previous_application_keys_ready = false;
};

enum class QuicLossTimerMode : std::uint8_t {
    None,
    Lost,
    Pto,
};

// Loop-affine connection state: once constructed, all state transitions and
// timer heap operations run on Options::loop. A quiesced owner loop is handled
// separately during destruction.
class QuicConnection : public common::NonCopyable, public common::NonMovable {
public:
    using DestroyCallback = void (*)(void *owner, QuicConnection &connection) noexcept;

    class Lease {
    public:
        Lease() noexcept = default;
        explicit Lease(QuicConnection *connection) noexcept;

        Lease(const Lease &) = delete;
        Lease &operator=(const Lease &) = delete;

        Lease(Lease &&other) noexcept : connection_(other.connection_) { other.connection_ = nullptr; }

        Lease &operator=(Lease &&other) noexcept {
            if (this == &other) {
                return *this;
            }
            reset();
            connection_ = other.connection_;
            other.connection_ = nullptr;
            return *this;
        }

        ~Lease() { reset(); }

        void reset() noexcept;
        [[nodiscard]] QuicConnection *release_raw() noexcept {
            QuicConnection *connection = connection_;
            connection_ = nullptr;
            return connection;
        }
        [[nodiscard]] QuicConnection *get() const noexcept { return connection_; }
        [[nodiscard]] QuicConnection &operator*() const noexcept { return *connection_; }
        [[nodiscard]] QuicConnection *operator->() const noexcept { return connection_; }
        [[nodiscard]] explicit operator bool() const noexcept { return connection_ != nullptr; }

        [[nodiscard]] static Lease adopt(QuicConnection *connection) noexcept {
            Lease lease;
            lease.connection_ = connection;
            return lease;
        }

    private:
        QuicConnection *connection_ = nullptr;
    };

    struct Ops {
        QuicStream::Lease (*create_stream)(void *owner, std::uint64_t stream_id) noexcept = nullptr;
        void (*on_peer_stream_attached)(void *owner, QuicStream &stream) noexcept = nullptr;
    };

    struct EndpointIndex {
        QuicConnection *connection = nullptr;
        Lease lease{};
        common::IntrusiveListHook link{};
    };

    struct SendQueueEntry {
        QuicConnection *connection = nullptr;
        common::IntrusiveListHook link{};
    };

    struct Options {
        QuicConnectionRole role = QuicConnectionRole::Server;
        net::SocketAddress local_addr{};
        net::SocketAddress remote_addr{};
        QuicConnectionId original_destination_connection_id{};
        QuicConnectionId initial_destination_connection_id{};
        QuicConnectionId local_connection_id{};
        QuicConnectionId remote_connection_id{};
        QuicConnectionId retry_source_connection_id{};
        QuicTransportSettings transport{};
        std::chrono::milliseconds keepalive_interval{0};
        QuicRecvFlowControlSettings recv_flow{};
        std::uint64_t max_peer_bidirectional_streams = kQuicDefaultMaxBidirectionalStreams;
        std::uint64_t max_peer_unidirectional_streams = kQuicDefaultMaxUnidirectionalStreams;
        std::uint64_t max_local_bidirectional_streams = kQuicDefaultMaxBidirectionalStreams;
        std::uint64_t max_local_unidirectional_streams = kQuicDefaultMaxUnidirectionalStreams;
        QuicOutputFramePool *output_frame_pool = nullptr;
        event::EventLoop *loop = nullptr;
        void *destroy_owner = nullptr;
        DestroyCallback on_destroy = nullptr;
        void *owner = nullptr;
        Ops ops{};
        // Default grace period for graceful shutdown. Applies when shutdown()/
        // shutdown_application() is called with grace == 0. After the period the
        // connection is forced into Closing regardless of in-flight streams.
        std::chrono::milliseconds graceful_shutdown_grace{30000};
        bool has_retry_source_connection_id = false;
        bool initial_path_validated = false;
        bool enable_early_data = false;
        // Server TLS context used to lazily create the SSL object only after
        // the first Initial packet passes AEAD authentication (mirrors nginx
        // ngx_quic_init_connection, which runs after ngx_quic_decrypt).
        net::TlsServerContext *tls_context = nullptr;
    };

    explicit QuicConnection(const Options &options) noexcept;
    ~QuicConnection();

    [[nodiscard]] QuicConnectionRole role() const noexcept { return options_.role; }
    [[nodiscard]] QuicConnectionState state() const noexcept { return state_; }
    [[nodiscard]] bool early_data_enabled() const noexcept { return options_.enable_early_data; }
    [[nodiscard]] const net::SocketAddress &local_addr() const noexcept { return options_.local_addr; }
    [[nodiscard]] const net::SocketAddress &remote_addr() const noexcept { return options_.remote_addr; }
    [[nodiscard]] const QuicConnectionId &original_destination_connection_id() const noexcept {
        return options_.original_destination_connection_id;
    }
    [[nodiscard]] const QuicConnectionId &initial_destination_connection_id() const noexcept {
        return options_.initial_destination_connection_id;
    }
    [[nodiscard]] const QuicConnectionId &local_connection_id() const noexcept { return options_.local_connection_id; }
    [[nodiscard]] const QuicConnectionId &remote_connection_id() const noexcept {
        return options_.remote_connection_id;
    }
    [[nodiscard]] bool retried() const noexcept { return options_.has_retry_source_connection_id; }
    [[nodiscard]] const QuicConnectionId &retry_source_connection_id() const noexcept {
        return options_.retry_source_connection_id;
    }
    [[nodiscard]] QuicErrorCode close_error() const noexcept {
        return static_cast<QuicErrorCode>(close_info_.error_code);
    }
    [[nodiscard]] QuicCloseSource close_source() const noexcept { return close_info_.source; }
    [[nodiscard]] bool closed() const noexcept { return state_ == QuicConnectionState::Closed; }
    [[nodiscard]] bool terminal_closing() const noexcept {
        return state_ == QuicConnectionState::Draining || state_ == QuicConnectionState::Closing ||
               state_ == QuicConnectionState::Closed;
    }
    [[nodiscard]] bool closing() const noexcept { return terminal_closing(); }
    [[nodiscard]] bool graceful_closing() const noexcept { return state_ == QuicConnectionState::GracefulClosing; }
    [[nodiscard]] bool attached_to_endpoint() const noexcept { return attached_to_endpoint_; }
    [[nodiscard]] bool detached_from_endpoint() const noexcept { return detached_from_endpoint_; }
    [[nodiscard]] std::uint32_t ref_count() const noexcept { return ref_count_; }
    [[nodiscard]] Lease lease() noexcept { return Lease(this); }

    common::IoResult<void> start_handshake() noexcept;
    common::IoResult<void> mark_established() noexcept;
    void begin_draining(QuicErrorCode error = QuicErrorCode::NoError) noexcept;
    void begin_draining(QuicCloseInfo info) noexcept;
    // RFC 9000 §10.2 Immediate Close — transport error path.
    // Sets state to Closing, queues a CONNECTION_CLOSE frame on every encryption level
    // for which write keys are available, then arms the 3*PTO close timer.
    void close(QuicErrorCode error = QuicErrorCode::NoError, std::uint64_t frame_type = 0) noexcept;
    // Like close() but skips the 3*PTO close timer — transitions to Closed immediately
    // after queuing CC frames and scheduling the send. For fatal errors where waiting
    // 3*PTO only delays cleanup (mirrors nginx's rc == NGX_ERROR path).
    void close_immediately(QuicErrorCode error = QuicErrorCode::NoError, std::uint64_t frame_type = 0) noexcept;
    // RFC 9000 §10.2 Immediate Close — application error path.
    // Identical to close() but uses CONNECTION_CLOSE_APP on Application-level packets and
    // accepts the full uint64 application error space. Initial/Handshake levels still
    // carry CONNECTION_CLOSE with error_code = APPLICATION_ERROR (0x0C) per RFC 9000 §10.2.3.
    void close_application(std::uint64_t error_code) noexcept;
    // RFC 9000 §20.1 — close with a CRYPTO_ERROR transport code derived from a
    // TLS alert (0x0100 | alert). Used when the TLS handshake raises a fatal
    // alert (captured by the QUIC TLS send_alert callback). Mirrors nginx's
    // NGX_QUIC_ERR_CRYPTO(alert) path: a transport-level CONNECTION_CLOSE is
    // queued on every encryption level with available write keys, and the close
    // is immediate (no 3*PTO linger) since the handshake is unrecoverable.
    // `frame_type` defaults to 0, matching nginx (it leaves error_ftype unset
    // for crypto alerts).
    void close_crypto_error(std::uint8_t alert, std::uint64_t frame_type = 0) noexcept;
    // Graceful shutdown: stop accepting new streams, let in-flight streams finish,
    // then transition to Closing once active_stream_count() reaches zero. If the
    // grace period (`grace`, or `options_.graceful_shutdown_grace` when 0) elapses
    // first, the connection is forced into Closing immediately. While shutting down,
    // the connection still processes packets, runs ACK / loss recovery / key update
    // normally. shutdown() prepares a transport-level CONNECTION_CLOSE; calling it
    // a second time is a no-op (state is not refreshed, grace is not extended).
    void shutdown(QuicErrorCode error = QuicErrorCode::NoError, std::uint64_t frame_type = 0,
                  std::chrono::milliseconds grace = std::chrono::milliseconds{0}) noexcept;
    // Like shutdown() but prepares a CONNECTION_CLOSE_APP, carrying `error_code`
    // in the application error space (RFC 9000 §10.2.3).
    void shutdown_application(std::uint64_t error_code,
                              std::chrono::milliseconds grace = std::chrono::milliseconds{0}) noexcept;
    [[nodiscard]] bool shutting_down() const noexcept { return graceful_closing(); }
    [[nodiscard]] bool accepting_new_streams() const noexcept {
        return state_ != QuicConnectionState::GracefulClosing && !terminal_closing();
    }
    void mark_closed() noexcept;
    // RFC 9000 §10.2.1 — when a packet arrives in Closing state, requeue a CC frame
    // on the level the packet was received (rate-limited to 1s). Called from the
    // packet processor.
    void requeue_close_frame(QuicEncryptionLevel level) noexcept;
    [[nodiscard]] std::chrono::milliseconds last_cc_msec() const noexcept { return last_cc_msec_; }

    [[nodiscard]] common::IoResult<std::uint64_t> next_local_stream_id(QuicStreamType type) noexcept;
    [[nodiscard]] bool can_accept_peer_stream(std::uint64_t stream_id) const noexcept;
    common::IoResult<void> record_peer_stream_id(std::uint64_t stream_id) noexcept;
    [[nodiscard]] QuicStream *find_stream(std::uint64_t stream_id) noexcept;
    [[nodiscard]] const QuicStream *find_stream(std::uint64_t stream_id) const noexcept;
    [[nodiscard]] std::size_t active_stream_count() const noexcept { return streams_.size(); }
    [[nodiscard]] common::IoResult<QuicStream *> try_attach_local_stream(QuicStream::Lease &&stream,
                                                                         QuicStreamType type) noexcept;
    [[nodiscard]] async::Task<common::IoResult<QuicStream *>>
    attach_local_stream(QuicStream::Lease stream, QuicStreamType type,
                        std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    [[nodiscard]] common::IoResult<QuicStream *> get_or_create_peer_stream(std::uint64_t stream_id) noexcept;
    [[nodiscard]] common::IoResult<void> recv_stream_frame(const QuicStreamFrame &frame, mem::IoBuf data) noexcept;
    [[nodiscard]] common::IoResult<void> recv_reset_stream_frame(const QuicResetStreamFrame &frame) noexcept;
    [[nodiscard]] common::IoResult<void> recv_stop_sending_frame(const QuicStopSendingFrame &frame) noexcept;
    [[nodiscard]] common::IoResult<void> recv_max_stream_data_frame(const QuicMaxStreamDataFrame &frame) noexcept;
    [[nodiscard]] common::IoResult<void> recv_max_streams_frame(const QuicMaxStreamsFrame &frame) noexcept;
    [[nodiscard]] common::IoResult<void> recv_max_data_frame(const QuicMaxDataFrame &frame) noexcept;
    [[nodiscard]] common::IoResult<void> recv_streams_blocked_frame(const QuicStreamsBlockedFrame &frame) noexcept;
    [[nodiscard]] event::EventLoop *loop() noexcept { return loop_; }
    [[nodiscard]] const event::EventLoop *loop() const noexcept { return loop_; }
    [[nodiscard]] mem::IoBufNodePool &recv_extent_pool() noexcept { return loop_->io_buf_node_pool(); }
    common::IoResult<void> set_app_ops(void *owner, const Ops &ops) noexcept;
    void drop_stream_send_ticket(std::uint64_t stream_id) noexcept;
    [[nodiscard]] std::uint64_t recv_data_consumed() const noexcept { return recv_data_consumed_; }
    [[nodiscard]] std::uint64_t recv_data_limit() const noexcept { return recv_data_limit_; }
    [[nodiscard]] std::uint64_t peer_max_data() const noexcept { return peer_max_data_; }
    [[nodiscard]] std::uint64_t peer_data_reserved() const noexcept { return peer_data_reserved_; }
    [[nodiscard]] std::uint64_t peer_data_available() const noexcept;
    [[nodiscard]] bool should_retransmit_data_blocked(std::uint64_t limit) const noexcept;
    [[nodiscard]] bool should_retransmit_stream_data_blocked(std::uint64_t stream_id,
                                                             std::uint64_t limit) const noexcept;
    [[nodiscard]] bool should_retransmit_max_streams(QuicStreamType type, std::uint64_t limit) const noexcept;
    [[nodiscard]] bool should_retransmit_streams_blocked(QuicStreamType type, std::uint64_t limit) const noexcept;
    [[nodiscard]] common::IoResult<void> on_stream_send_acked(std::uint64_t stream_id, std::size_t offset,
                                                              std::size_t length, bool fin) noexcept;
    [[nodiscard]] common::IoResult<void> on_stream_send_failed(std::uint64_t stream_id, std::size_t offset,
                                                               std::size_t length, bool fin) noexcept;

    [[nodiscard]] bool is_local_stream(std::uint64_t stream_id) const noexcept;
    [[nodiscard]] bool is_peer_stream(std::uint64_t stream_id) const noexcept { return !is_local_stream(stream_id); }
    [[nodiscard]] static bool is_bidirectional_stream(std::uint64_t stream_id) noexcept;
    [[nodiscard]] static bool is_unidirectional_stream(std::uint64_t stream_id) noexcept;
    [[nodiscard]] static QuicStreamType stream_type(std::uint64_t stream_id) noexcept;

    [[nodiscard]] QuicPacketNumberSpace &packet_number_space(QuicEncryptionLevel level) noexcept;
    [[nodiscard]] const QuicPacketNumberSpace &packet_number_space(QuicEncryptionLevel level) const noexcept;
    [[nodiscard]] static std::size_t packet_number_space_index(QuicEncryptionLevel level) noexcept;
    [[nodiscard]] QuicCryptoState &crypto() noexcept { return crypto_; }
    [[nodiscard]] const QuicCryptoState &crypto() const noexcept { return crypto_; }
    [[nodiscard]] bool key_phase() const noexcept { return key_phase_; }
    [[nodiscard]] bool next_keys_ready() const noexcept { return crypto_.next_application_keys_ready; }
    void flip_key_phase() noexcept { key_phase_ = !key_phase_; }
    void arm_key_update_discard_timer() noexcept;
    void cancel_key_update_discard_timer() noexcept;
    void on_packet_processed() noexcept;
    void on_ack_eliciting_packet_sent() noexcept;
    [[nodiscard]] std::chrono::milliseconds effective_idle_timeout() const noexcept;
    [[nodiscard]] bool idle_timer_armed() const noexcept { return idle_timer_entry_.is_in_heap(); }
    [[nodiscard]] bool close_timer_armed() const noexcept { return close_timer_entry_.is_in_heap(); }
    [[nodiscard]] bool keepalive_timer_armed() const noexcept { return keepalive_timer_entry_.is_in_heap(); }
    [[nodiscard]] bool pacing_timer_armed() const noexcept { return pacing_timer_entry_.is_in_heap(); }
    [[nodiscard]] bool idle_send_timer_set() const noexcept { return idle_send_timer_set_; }
    void arm_idle_timer() noexcept;
    void cancel_idle_timer() noexcept;
    void arm_close_timer() noexcept;
    void arm_close_timer_immediate() noexcept;
    void cancel_close_timer() noexcept;
    void arm_keepalive_timer() noexcept;
    void cancel_keepalive_timer() noexcept;
    void cancel_all_timers() noexcept;
    [[nodiscard]] QuicCongestionState &congestion() noexcept { return congestion_; }
    [[nodiscard]] const QuicCongestionState &congestion() const noexcept { return congestion_; }
    [[nodiscard]] QuicRttState &rtt() noexcept { return rtt_; }
    [[nodiscard]] const QuicRttState &rtt() const noexcept { return rtt_; }
    [[nodiscard]] std::uint64_t reset_packet_number() const noexcept { return reset_packet_number_; }
    [[nodiscard]] std::uint32_t pto_count() const noexcept { return pto_count_; }
    [[nodiscard]] QuicLossTimerMode loss_timer_mode() const noexcept { return loss_timer_mode_; }
    [[nodiscard]] bool loss_timer_armed() const noexcept { return loss_timer_entry_.is_in_heap(); }
    void reset_pto_count() noexcept { pto_count_ = 0; }
    void arm_loss_detection_timer() noexcept;
    void cancel_loss_detection_timer() noexcept;
    void reset_congestion_for_path(QuicTime now) noexcept;

    [[nodiscard]] QuicPathManager &paths() noexcept { return path_manager_; }
    [[nodiscard]] const QuicPathManager &paths() const noexcept { return path_manager_; }

    // Thin forwarders preserved for external callers.
    void arm_path_validation_timer() noexcept { path_manager_.arm_validation_timer(); }
    void cancel_path_validation_timer() noexcept { path_manager_.cancel_validation_timer(); }
    [[nodiscard]] QuicPath *active_path() noexcept { return path_manager_.active(); }
    [[nodiscard]] const QuicPath *active_path() const noexcept { return path_manager_.active(); }
    [[nodiscard]] std::size_t path_count() const noexcept { return path_manager_.count(); }
    [[nodiscard]] QuicPath *find_path(const net::SocketAddress &remote, const net::SocketAddress &local) noexcept {
        return path_manager_.find(remote, local);
    }
    [[nodiscard]] const QuicPath *find_path(const net::SocketAddress &remote,
                                            const net::SocketAddress &local) const noexcept {
        return path_manager_.find(remote, local);
    }
    [[nodiscard]] QuicPath *find_path(QuicPathTag tag) noexcept { return path_manager_.find(tag); }
    [[nodiscard]] const QuicPath *find_path(QuicPathTag tag) const noexcept { return path_manager_.find(tag); }
    [[nodiscard]] QuicPath *create_path(const net::SocketAddress &remote, const net::SocketAddress &local,
                                        const QuicConnectionId &remote_connection_id, QuicPathTag tag) noexcept {
        return path_manager_.create(remote, local, remote_connection_id, tag);
    }
    void free_path(QuicPath &path) noexcept { path_manager_.free(path); }
    [[nodiscard]] bool set_active_path(QuicPath &path) noexcept { return path_manager_.set_active(path); }
    void record_path_received(QuicPath &path, std::size_t len) noexcept { path_manager_.record_received(path, len); }
    void record_path_sent(QuicPath &path, std::size_t len) noexcept { path_manager_.record_sent(path, len); }
    [[nodiscard]] bool has_path_send_work() const noexcept { return path_manager_.has_send_work(); }
    [[nodiscard]] common::IoResult<void> recv_path_challenge_frame(QuicPath &path,
                                                                   const QuicPathChallengeFrame &frame) noexcept {
        return path_manager_.recv_path_challenge_frame(path, frame);
    }
    [[nodiscard]] common::IoResult<bool> recv_path_response_frame(const QuicPathChallengeFrame &frame,
                                                                  QuicTime now) noexcept {
        return path_manager_.recv_path_response_frame(frame, now);
    }
    [[nodiscard]] common::IoResult<QuicPath *> recv_path_response_frame_with_path(const QuicPathChallengeFrame &frame,
                                                                                  QuicTime now) noexcept {
        return path_manager_.recv_path_response_frame_with_path(frame, now);
    }
    [[nodiscard]] common::IoResult<void> handle_migration(QuicPath &path, bool rebound, QuicTime now) noexcept {
        return path_manager_.handle_migration(path, rebound, now);
    }
    [[nodiscard]] static std::size_t path_send_limit(const QuicPath &path, std::size_t size) noexcept {
        return QuicPathManager::send_limit(path, size);
    }

    [[nodiscard]] QuicTlsSession &tls() noexcept { return tls_; }
    [[nodiscard]] const QuicTlsSession &tls() const noexcept { return tls_; }
    common::IoResult<void> init_initial_crypto(const QuicConnectionId &original_dcid) noexcept;
    // Lazily create the server SSL object the first time an Initial packet
    // is authenticated. Idempotent; no-op when no TLS context is configured
    // (e.g. tests). Deferring SSL_new past AEAD auth avoids per-forged-packet
    // TLS setup cost (DoS hardening, audit #4).
    [[nodiscard]] common::IoResult<void> ensure_server_tls() noexcept;
    common::IoResult<void> apply_peer_transport_params(const QuicTransportParams &params) noexcept;
    [[nodiscard]] common::IoResult<bool> recv_retire_connection_id_frame(const QuicRetireConnectionIdFrame &frame,
                                                                         const QuicConnectionId &packet_dcid) noexcept;
    // Handle a peer-issued NEW_CONNECTION_ID frame (RFC 9000 §19.15). On success
    // returns whether new outbound traffic was queued (a RETIRE_CONNECTION_ID
    // frame in response, or a path-CID switch). Protocol-level violations
    // close the connection and propagate as IoErr::Invalid.
    [[nodiscard]] common::IoResult<bool> recv_new_connection_id_frame(const QuicNewConnectionIdFrame &frame) noexcept;
    [[nodiscard]] bool should_retransmit_new_connection_id(std::uint64_t sequence_number) const noexcept;
    [[nodiscard]] bool should_retransmit_retire_connection_id(std::uint64_t sequence_number) const noexcept;
    [[nodiscard]] bool has_active_local_connection_id(const QuicConnectionId &cid) const noexcept;
    [[nodiscard]] common::IoResult<void>
    stateless_reset_token_for(const QuicConnectionId &cid, std::uint8_t out[kStatelessResetTokenLength]) const noexcept;
    // RFC 9000 §10.3: detect a peer-sent stateless reset. A short-header (1-RTT)
    // packet that fails to decrypt is a reset iff its final 16 bytes equal a
    // stateless_reset_token the peer advertised via NEW_CONNECTION_ID. Compares
    // the trailing 16 bytes of the supplied packet against every active
    // peer-issued (remote) Connection ID's token in constant time. The caller is
    // responsible for having already established that the packet is a short
    // header that failed to decrypt.
    [[nodiscard]] bool detects_stateless_reset(const std::uint8_t *packet_data, std::size_t packet_len) const noexcept;
    [[nodiscard]] const QuicTransportSettings &local_transport() const noexcept { return options_.transport; }
    [[nodiscard]] const QuicPeerTransportState &peer_transport() const noexcept { return peer_transport_; }
    [[nodiscard]] bool peer_transport_params_received() const noexcept { return peer_transport_.received; }
    EndpointIndex endpoint_index{};
    QuicConnectionIdIndex original_dcid_index{};
    SendQueueEntry send_queue_entry{};

private:
    class LocalStreamAttachAwaiter;

    struct PeerStreamLimitWindow {
        std::uint64_t concurrent_limit = 0;
        std::uint64_t opened_count = 0;
        std::uint64_t retired_count = 0;
        std::uint64_t advertised_limit = 0;
    };

    struct LocalStreamBlockedState {
        std::uint64_t last_limit = 0;
        bool reported = false;
    };

    [[nodiscard]] std::uint8_t local_initiator_bit() const noexcept;
    [[nodiscard]] PeerStreamLimitWindow &peer_stream_window(QuicStreamType type) noexcept;
    [[nodiscard]] const PeerStreamLimitWindow &peer_stream_window(QuicStreamType type) const noexcept;
    [[nodiscard]] LocalStreamBlockedState &local_stream_blocked_state(QuicStreamType type) noexcept;
    [[nodiscard]] const LocalStreamBlockedState &local_stream_blocked_state(QuicStreamType type) const noexcept;
    [[nodiscard]] std::uint64_t local_stream_limit(QuicStreamType type) const noexcept;
    [[nodiscard]] std::uint64_t peer_stream_limit(QuicStreamType type) const noexcept;
    [[nodiscard]] bool local_stream_blocked(QuicStreamType type) const noexcept;
    [[nodiscard]] bool local_stream_attach_ready(QuicStreamType type) const noexcept;
    [[nodiscard]] bool is_gone_peer_stream(std::uint64_t stream_id) const noexcept;
    // RFC 9000 §4.6: a peer-initiated stream whose sequence (id >> 2) reaches or
    // exceeds the advertised max_streams has exceeded the limit advertised via
    // MAX_STREAMS — a STREAM_LIMIT_ERROR peer violation. This is distinct from
    // the concurrent-active-stream window (can_accept_peer_stream's second
    // check), which is a non-fatal flow-control gate and must NOT close the
    // connection.
    [[nodiscard]] bool peer_stream_exceeds_advertised_limit(std::uint64_t stream_id) const noexcept;
    [[nodiscard]] common::IoResult<QuicStream *> attach_stream(QuicStream::Lease &&lease, std::uint64_t stream_id,
                                                               QuicStreamRecvQueue::Options recv_options,
                                                               bool local_initiated) noexcept;
    // Invoke the app's create_stream callback, attach the stream to this
    // connection, insert it into the stream table, then notify the app. Used for
    // the target peer stream AND every implicitly-opened intermediate peer
    // stream (RFC 9000 §2.1). Precondition: create_stream is set and stream_id
    // is a peer stream within the advertised limit.
    [[nodiscard]] common::IoResult<QuicStream *> create_peer_stream(std::uint64_t stream_id) noexcept;
    void on_peer_stream_retired(std::uint64_t stream_id) noexcept;
    void maybe_extend_peer_stream_limit(QuicStreamType type) noexcept;
    void retire_stream(QuicStream &stream) noexcept;
    void try_release_stream(QuicStream &stream) noexcept;
    [[nodiscard]] common::IoResult<void> queue_stream_frame(QuicStream &stream) noexcept;
    void schedule_send() noexcept;
    [[nodiscard]] common::IoResult<void> queue_reset_stream_frame(std::uint64_t stream_id, std::uint64_t error_code,
                                                                  std::uint64_t final_size) noexcept;
    [[nodiscard]] common::IoResult<void> queue_stop_sending_frame(std::uint64_t stream_id,
                                                                  std::uint64_t error_code) noexcept;
    [[nodiscard]] common::IoResult<void> queue_max_stream_data_frame(std::uint64_t stream_id,
                                                                     std::uint64_t limit) noexcept;
    [[nodiscard]] common::IoResult<void> queue_max_streams_frame(QuicStreamType type, std::uint64_t limit) noexcept;
    [[nodiscard]] common::IoResult<void> queue_max_data_frame(std::uint64_t limit) noexcept;
    [[nodiscard]] common::IoResult<void> queue_streams_blocked_frame(QuicStreamType type, std::uint64_t limit) noexcept;
    [[nodiscard]] common::IoResult<void> queue_data_blocked_frame(std::uint64_t limit) noexcept;
    [[nodiscard]] common::IoResult<void> queue_stream_data_blocked_frame(QuicStream &stream,
                                                                         std::uint64_t limit) noexcept;
    [[nodiscard]] common::IoResult<void> queue_ping_frame() noexcept;
    [[nodiscard]] std::size_t active_local_connection_id_count() const noexcept;
    [[nodiscard]] std::size_t local_connection_id_target() const noexcept;
    [[nodiscard]] QuicLocalConnectionIdSlot *find_local_connection_id_slot(std::uint64_t sequence_number) noexcept;
    [[nodiscard]] const QuicLocalConnectionIdSlot *
    find_local_connection_id_slot(std::uint64_t sequence_number) const noexcept;
    [[nodiscard]] QuicLocalConnectionIdSlot *find_free_local_connection_id_slot() noexcept;
    [[nodiscard]] common::IoResult<void>
    queue_new_connection_id_frame(const QuicLocalConnectionIdSlot &slot,
                                  const std::uint8_t token[kStatelessResetTokenLength]) noexcept;
    [[nodiscard]] common::IoResult<void> queue_retire_connection_id_frame(std::uint64_t sequence_number) noexcept;
    [[nodiscard]] QuicRemoteConnectionIdSlot *find_remote_connection_id_slot(std::uint64_t sequence_number) noexcept;
    [[nodiscard]] const QuicRemoteConnectionIdSlot *
    find_remote_connection_id_slot(std::uint64_t sequence_number) const noexcept;
    [[nodiscard]] QuicRemoteConnectionIdSlot *find_free_remote_connection_id_slot() noexcept;
    [[nodiscard]] std::size_t active_remote_connection_id_count() const noexcept;
    // Drop a single remote-CID slot, posting the corresponding RETIRE frame and
    // — if the CID is bound to a path — either swapping to an unused CID
    // (active path) or releasing the path (non-active). Returns true when any
    // outbound frame was queued.
    [[nodiscard]] common::IoResult<bool> retire_remote_connection_id(QuicRemoteConnectionIdSlot &slot) noexcept;
    // Queue a single CONNECTION_CLOSE / CONNECTION_CLOSE_APP frame in `level`'s pending
    // queue. Picks the correct frame type from close_info_ and the level. No-op when
    // write keys for that level are not yet derived. Returns whether a frame was
    // actually queued.
    [[nodiscard]] bool queue_close_frame_for_level(QuicEncryptionLevel level) noexcept;
    void enqueue_close_frames_all_levels() noexcept;
    void clear_pending_frames_all_levels() noexcept;
    void close_all_streams(std::uint64_t error_code) noexcept;
    void clear_frames_for_detach() noexcept;
    void clear_packet_space_frames_for_detach(QuicPacketNumberSpace &space) noexcept;
    void enter_graceful_closing(QuicCloseInfo info, std::chrono::milliseconds grace) noexcept;
    void enter_closing(QuicCloseInfo info, bool immediate = false) noexcept;
    void enter_draining(QuicCloseInfo info) noexcept;
    void enter_closed() noexcept;
    void maybe_finish_graceful_close() noexcept;
    void assert_loop_affinity() const noexcept;
    [[nodiscard]] event::EventLoop *active_timer_loop() const noexcept;
    void arm_pacing_timer(std::chrono::steady_clock::time_point deadline) noexcept;
    void cancel_pacing_timer() noexcept;
    void cancel_all_timers_quiesced() noexcept;
    [[nodiscard]] bool has_pending_send_work() const noexcept;
    [[nodiscard]] bool has_pacing_exempt_send_work() const noexcept;
    [[nodiscard]] std::chrono::milliseconds keepalive_delay() const noexcept;
    [[nodiscard]] bool reserve_peer_data(std::uint64_t bytes) noexcept;
    [[nodiscard]] std::uint64_t initial_stream_send_limit(std::uint64_t stream_id) const noexcept;
    void wait_for_peer_data(QuicStream::WriteAwaiter &awaiter) noexcept;
    void cancel_peer_data_wait(QuicStream::WriteAwaiter &awaiter) noexcept;
    void notify_peer_data_waiters(common::IoErr result = common::IoErr::None) noexcept;
    void wait_for_local_stream_attach(LocalStreamAttachAwaiter &awaiter) noexcept;
    void cancel_local_stream_attach_wait(LocalStreamAttachAwaiter &awaiter) noexcept;
    void notify_local_stream_attach_waiters(QuicStreamType type, common::IoErr result = common::IoErr::None) noexcept;
    void notify_all_local_stream_attach_waiters(common::IoErr result = common::IoErr::None) noexcept;
    void attach_to_endpoint(QuicUdpEndpoint &endpoint) noexcept;
    void detach_from_endpoint() noexcept;
    void retain() noexcept;
    void release() noexcept;
    [[nodiscard]] bool ready_for_destruction() const noexcept;
    [[nodiscard]] bool can_queue_frame() const noexcept;
    [[nodiscard]] common::IoResult<void> check_recv_data_delta(std::uint64_t delta) const noexcept;
    void commit_recv_data_delta(std::uint64_t delta) noexcept;
    void maybe_extend_recv_data_flow_control() noexcept;
    static void on_loss_detection_timer(QuicConnection *connection) noexcept;
    static void on_key_update_discard_timer(QuicConnection *connection) noexcept;
    static void on_idle_timer(QuicConnection *connection) noexcept;
    static void on_close_timer(QuicConnection *connection) noexcept;
    static void on_keepalive_timer(QuicConnection *connection) noexcept;
    static void on_pacing_timer(QuicConnection *connection) noexcept;

    friend class QuicPathManager;
    friend class QuicSendScheduler;

    Options options_{};
    event::EventLoop *loop_ = nullptr;
    QuicUdpEndpoint *endpoint_ = nullptr;
    QuicConnectionState state_ = QuicConnectionState::Init;
    std::uint64_t next_local_bidi_stream_id_ = 0;
    std::uint64_t next_local_uni_stream_id_ = 0;
    PeerStreamLimitWindow peer_bidi_streams_{};
    PeerStreamLimitWindow peer_uni_streams_{};
    LocalStreamBlockedState local_bidi_streams_blocked_{};
    LocalStreamBlockedState local_uni_streams_blocked_{};
    QuicOutputFramePool output_frame_pool_{};
    std::array<QuicPacketNumberSpace, kQuicPacketNumberSpaceCount> packet_number_spaces_{};
    QuicCongestionState congestion_{};
    QuicRttState rtt_{};
    std::uint64_t reset_packet_number_ = 0;
    std::uint32_t pto_count_ = 0;
    QuicLossTimerMode loss_timer_mode_ = QuicLossTimerMode::None;
    event::EventLoop::TimerEntry loss_timer_entry_{};
    event::EventLoop::TimerEntry key_update_discard_timer_entry_{};
    event::EventLoop::TimerEntry idle_timer_entry_{};
    event::EventLoop::TimerEntry close_timer_entry_{};
    event::EventLoop::TimerEntry keepalive_timer_entry_{};
    event::EventLoop::TimerEntry pacing_timer_entry_{};
    QuicPacerState pacer_{};
    QuicCryptoState crypto_{};
    QuicPeerTransportState peer_transport_{};
    QuicStreamTable streams_{};
    QuicTlsSession tls_{};
    QuicPathManager path_manager_{*this};
    std::array<QuicLocalConnectionIdSlot, kQuicLocalConnectionIdSlotCount> local_cids_{};
    std::uint64_t next_local_cid_sequence_ = 1;
    std::array<QuicRemoteConnectionIdSlot, kQuicRemoteConnectionIdSlotCount> remote_cids_{};
    // Largest retire_prior_to value we have observed; per RFC 9000 §19.15
    // smaller subsequent values MUST be ignored, and CIDs below it that have
    // not yet been retired MUST be retired.
    std::uint64_t max_retired_remote_seq_ = 0;
    // Largest sequence_number ever installed (locally accepted) into the pool.
    // Tracks the boundary used to detect peer-side replays after retirement
    // (RFC 9000 §19.15 — receipt of NEW_CONNECTION_ID with sequence_number
    // smaller than Retire Prior To MUST be RETIRE-acknowledged).
    std::uint64_t largest_seen_remote_seq_ = 0;
    std::uint64_t recv_data_consumed_ = 0;
    std::uint64_t recv_data_limit_ = 0;
    std::uint64_t peer_max_data_ = 0;
    std::uint64_t peer_data_reserved_ = 0;
    std::uint64_t last_data_blocked_limit_ = 0;
    common::IntrusiveListHook *peer_data_wait_head_ = nullptr;
    common::IntrusiveListHook *peer_data_wait_tail_ = nullptr;
    common::IntrusiveListHook *local_bidi_stream_attach_wait_head_ = nullptr;
    common::IntrusiveListHook *local_bidi_stream_attach_wait_tail_ = nullptr;
    common::IntrusiveListHook *local_uni_stream_attach_wait_head_ = nullptr;
    common::IntrusiveListHook *local_uni_stream_attach_wait_tail_ = nullptr;
    bool data_blocked_reported_ = false;
    bool key_phase_ = false;
    bool idle_send_timer_set_ = false;
    bool attached_to_endpoint_ = false;
    bool detached_from_endpoint_ = false;
    std::uint32_t ref_count_ = 1;
    void *destroy_owner_ = nullptr;
    DestroyCallback on_destroy_ = nullptr;
    // close_info_ is interpreted by state_: in GracefulClosing it is the staged
    // final close, in Closing it is the sent CONNECTION_CLOSE, and in Draining it
    // is the peer/stateless-reset reason. last_cc_msec_ rate-limits close-frame
    // requeueing in Closing.
    QuicCloseInfo close_info_{};
    std::chrono::milliseconds last_cc_msec_{0};

    friend class QuicStream;
    friend class QuicStream::WriteAwaiter;
    friend class QuicUdpEndpoint;
};

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_CONNECTION_H
