#ifndef FIBER_QUIC_QUIC_CONNECTION_H
#define FIBER_QUIC_QUIC_CONNECTION_H

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

#include <openssl/aead.h>
#include <openssl/aes.h>

#include "../async/LocalWaitGroup.h"
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
#include "QuicHandshakeGate.h"
#include "QuicPacer.h"
#include "QuicPacketNumberSpace.h"
#include "QuicPath.h"
#include "QuicPathManager.h"
#include "QuicStream.h"
#include "QuicStreamTable.h"
#include "QuicTlsSession.h"

struct ssl_session_st;
typedef struct ssl_session_st SSL_SESSION;

namespace fiber::quic {

struct QuicTransportParams;
struct QuicPacketHeader;
struct QuicReceivedDatagram;
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
inline constexpr std::size_t kQuicDefaultConnectionRetainedStorageLimit = 16ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t kQuicDefaultEndpointRetainedStorageLimit = 256ULL * 1024ULL * 1024ULL;
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
    std::size_t retained_storage_limit = kQuicDefaultConnectionRetainedStorageLimit;
};

struct QuicPeerTransportState {
    QuicTransportSettings params{};
    bool received = false;
};

enum class QuicConnectPhase : std::uint8_t {
    Endpoint,
    Connection,
    InitialCrypto,
    Tls,
    Handshake,
    VersionNegotiation,
    TransportParameters,
    Timeout,
    PeerClose,
};

struct QuicConnectError {
    QuicConnectPhase phase = QuicConnectPhase::Connection;
    common::IoErr io_error = common::IoErr::Unknown;
    QuicCloseInfo close{};
    long tls_verify_result = 0;
    std::uint8_t tls_alert = 0;
    std::uint32_t offered_version = 0;
};

// Consumed synchronously by QuicConnection::connect(). Every pointer and view
// is borrowed only until connect() returns: BoringSSL copies what it needs out
// of tls while the SSL is created, SSL_set_session retains its own reference,
// and the token and remembered transport are copied into connection state.
struct QuicClientConnectParams {
    net::TlsClientParam tls{};
    bool allow_insecure = false;
    SSL_SESSION *resumption_session = nullptr;
    const std::uint8_t *token = nullptr;
    std::size_t token_len = 0;
    // Offer 0-RTT. Early data is actually attempted only when
    // resumption_session and remembered_peer_transport are both present: the
    // remembered settings seed the peer limits until the real transport
    // parameters arrive and are re-validated against them (RFC 9000 §7.4.1).
    bool enable_early_data = false;
    const QuicTransportSettings *remembered_peer_transport = nullptr;
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

    QuicCryptoSuite suite = QuicCryptoSuite::InitialAes128GcmSha256;
    std::array<std::uint8_t, kQuicMaxSecretLength> secret{};
    std::array<std::uint8_t, kQuicIvLength> iv{};
    std::size_t secret_len = 0;
    std::size_t iv_len = 0;
    EVP_AEAD_CTX aead{};
    bool aead_initialized = false;
    bool ready = false;
};

struct QuicHeaderProtectionKeys : public common::NonCopyable, public common::NonMovable {
    QuicHeaderProtectionKeys() noexcept = default;
    ~QuicHeaderProtectionKeys();

    void reset() noexcept;

    std::array<std::uint8_t, kQuicMaxHeaderProtectionKeyLength> key{};
    std::size_t key_len = 0;
    AES_KEY aes_key{};
    bool chacha20 = false;
    bool ready = false;
};

struct QuicPacketProtectionKeyView {
    QuicPacketProtectionKeys *packet = nullptr;
    QuicHeaderProtectionKeys *header = nullptr;

    [[nodiscard]] explicit operator bool() const noexcept { return packet != nullptr && header != nullptr; }
    [[nodiscard]] bool ready() const noexcept { return *this && packet->ready && header->ready; }
};

struct QuicProtectionKeySlot : public common::NonCopyable, public common::NonMovable {
    QuicProtectionKeySlot() noexcept = default;
    ~QuicProtectionKeySlot() = default;

    void reset() noexcept {
        packet.reset();
        header.reset();
    }

    [[nodiscard]] QuicPacketProtectionKeyView view() noexcept { return {&packet, &header}; }

    QuicPacketProtectionKeys packet{};
    QuicHeaderProtectionKeys header{};
};

struct QuicTransientCryptoBlock : public common::NonCopyable, public common::NonMovable {
    QuicTransientCryptoBlock() noexcept = default;
    ~QuicTransientCryptoBlock() = default;

    void reset() noexcept;
    [[nodiscard]] bool empty() const noexcept;

    QuicProtectionKeySlot initial_read{};
    QuicProtectionKeySlot initial_write{};
    QuicProtectionKeySlot early_read{};
    QuicProtectionKeySlot early_write{};
    QuicProtectionKeySlot handshake_read{};
    QuicProtectionKeySlot handshake_write{};
    QuicTransientCryptoBlock *pool_next = nullptr;
};

struct QuicApplicationCryptoBlock : public common::NonCopyable, public common::NonMovable {
    QuicApplicationCryptoBlock() noexcept = default;
    ~QuicApplicationCryptoBlock() = default;

    void reset() noexcept;

    [[nodiscard]] QuicPacketProtectionKeyView current_read() noexcept;
    [[nodiscard]] QuicPacketProtectionKeyView next_read() noexcept;
    [[nodiscard]] QuicPacketProtectionKeyView previous_read() noexcept;
    [[nodiscard]] QuicPacketProtectionKeyView current_write() noexcept;
    [[nodiscard]] QuicPacketProtectionKeyView next_write() noexcept;
    void promote() noexcept;

    std::array<QuicPacketProtectionKeys, 3> read_keys{};
    std::array<QuicPacketProtectionKeys, 2> write_keys{};
    QuicHeaderProtectionKeys read_header{};
    QuicHeaderProtectionKeys write_header{};
    std::uint8_t current_read_index = 0;
    std::uint8_t next_read_index = 1;
    std::uint8_t previous_read_index = 2;
    std::uint8_t current_write_index = 0;
    std::uint8_t next_write_index = 1;
    QuicApplicationCryptoBlock *pool_next = nullptr;
};

inline constexpr std::size_t kQuicCryptoBlockPoolMaxCached = 64;

class QuicCryptoBlockPool : public common::NonCopyable, public common::NonMovable {
public:
    QuicCryptoBlockPool() noexcept = default;
    ~QuicCryptoBlockPool();

    [[nodiscard]] QuicTransientCryptoBlock *alloc_transient() noexcept;
    [[nodiscard]] QuicApplicationCryptoBlock *alloc_application() noexcept;
    void release(QuicTransientCryptoBlock *block) noexcept;
    void release(QuicApplicationCryptoBlock *block) noexcept;

    [[nodiscard]] std::size_t active_blocks() const noexcept { return active_blocks_; }
    [[nodiscard]] std::size_t cached_blocks() const noexcept { return cached_blocks_; }
    [[nodiscard]] std::size_t allocation_misses() const noexcept { return allocation_misses_; }

private:
    QuicTransientCryptoBlock *transient_free_ = nullptr;
    QuicApplicationCryptoBlock *application_free_ = nullptr;
    std::size_t active_blocks_ = 0;
    std::size_t cached_blocks_ = 0;
    std::size_t allocation_misses_ = 0;
};

enum class QuicReadKeyEpoch : std::uint8_t {
    Previous,
    Current,
    Next,
};

struct QuicKeyEpochState {
    std::uint64_t generation = 0;
    std::uint64_t current_read_lowest_pn = UINT64_MAX;
    std::uint64_t current_write_first_pn = UINT64_MAX;
    std::uint64_t encrypted_packets = 0;
    std::uint64_t authentication_failures = 0;
    std::chrono::steady_clock::time_point previous_discard_deadline{};
    std::chrono::steady_clock::time_point next_update_not_before{};
    bool phase = false;
    bool handshake_confirmed = false;
    bool current_write_acked = false;
};

struct QuicCryptoState : public common::NonCopyable, public common::NonMovable {
    QuicCryptoState() noexcept = default;
    ~QuicCryptoState();

    void reset() noexcept;
    [[nodiscard]] common::IoResult<void> reset_initial_keys() noexcept;
    void set_block_pool(QuicCryptoBlockPool *pool) noexcept;
    [[nodiscard]] common::IoResult<void> ensure_transient() noexcept;
    [[nodiscard]] common::IoResult<void> ensure_application() noexcept;
    void discard_level(QuicEncryptionLevel level) noexcept;
    void discard_previous_application_read() noexcept;
    void promote_application_keys() noexcept;

    [[nodiscard]] QuicPacketProtectionKeyView initial_read() noexcept;
    [[nodiscard]] QuicPacketProtectionKeyView initial_write() noexcept;
    [[nodiscard]] QuicPacketProtectionKeyView early_read() noexcept;
    [[nodiscard]] QuicPacketProtectionKeyView early_write() noexcept;
    [[nodiscard]] QuicPacketProtectionKeyView handshake_read() noexcept;
    [[nodiscard]] QuicPacketProtectionKeyView handshake_write() noexcept;
    [[nodiscard]] QuicPacketProtectionKeyView application_read() noexcept;
    [[nodiscard]] QuicPacketProtectionKeyView application_write() noexcept;
    [[nodiscard]] QuicPacketProtectionKeyView next_application_read() noexcept;
    [[nodiscard]] QuicPacketProtectionKeyView next_application_write() noexcept;
    [[nodiscard]] QuicPacketProtectionKeyView previous_application_read() noexcept;
    [[nodiscard]] QuicPacketProtectionKeyView application_read(QuicReadKeyEpoch epoch) noexcept;
    [[nodiscard]] QuicReadKeyEpoch select_application_read_epoch(bool wire_phase,
                                                                 std::uint64_t packet_number) const noexcept;

    [[nodiscard]] bool initial_ready() noexcept { return initial_read().ready() && initial_write().ready(); }
    [[nodiscard]] bool next_application_keys_ready() noexcept {
        return next_application_read().ready() && next_application_write().ready();
    }
    [[nodiscard]] bool previous_application_keys_ready() noexcept { return previous_application_read().ready(); }
    [[nodiscard]] bool has_transient_block() const noexcept { return transient_ != nullptr; }
    [[nodiscard]] bool has_application_block() const noexcept { return application_ != nullptr; }
    [[nodiscard]] bool initial_discarded() const noexcept { return initial_discarded_; }
    [[nodiscard]] bool early_write_ready() const noexcept {
        return transient_ != nullptr && transient_->early_write.packet.ready && transient_->early_write.header.ready;
    }
    [[nodiscard]] bool application_write_ready() const noexcept {
        return application_ != nullptr && application_->write_keys[application_->current_write_index].ready &&
               application_->write_header.ready;
    }
    [[nodiscard]] QuicKeyEpochState &epoch() noexcept { return epoch_; }
    [[nodiscard]] const QuicKeyEpochState &epoch() const noexcept { return epoch_; }

private:
    void release_transient_if_empty() noexcept;
    void release_transient() noexcept;
    void release_application() noexcept;

    QuicCryptoBlockPool *pool_ = nullptr;
    QuicTransientCryptoBlock *transient_ = nullptr;
    QuicApplicationCryptoBlock *application_ = nullptr;
    QuicKeyEpochState epoch_{};
    bool initial_discarded_ = false;
};

enum class QuicLossTimerMode : std::uint8_t {
    None,
    Lost,
    Pto,
};

// Loop-affine connection state: once constructed, all state transitions and
// timer heap operations run on the hosting endpoint's loop.
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

    // Notifications run inline on the connection's loop and must not destroy
    // the owner or the connection, nor drive a nested event loop. State hooks
    // may close or shut down; nested state notifications fold into another pass.
    struct Ops {
        QuicStream::Lease (*create_stream)(void *owner, std::uint64_t stream_id) noexcept = nullptr;
        void (*on_peer_stream_attached)(void *owner, QuicStream &stream) noexcept = nullptr;
        void (*on_early_data_rejected)(void *owner) noexcept = nullptr;
        // state() moved and its stream/frame/timer bookkeeping is complete.
        // Read the new state from the connection. Endpoint detachment, which
        // can release the last lease, happens after the Closed notification.
        void (*on_state_change)(void *owner, QuicConnection &connection) noexcept = nullptr;
        // The number of live streams changed in either direction, or the peer
        // granted us more stream credit (MAX_STREAMS, or its initial transport
        // parameters). Deliberately carries no stream type or direction: an
        // owner re-reads what it actually tracks -- local_stream_attach_status()
        // per type, or its own count of application streams. QUIC cannot tell an
        // application stream from a protocol one, so an owner that cares about
        // that distinction must keep its own count.
        //
        // Not raised when we extend the peer's credit: that only ever happens as
        // a consequence of a peer stream retiring, and the extension is applied
        // before that retirement's notification runs.
        //
        // Not raised for a MAX_STREAMS frame that does not raise our limit --
        // RFC 9000 4.6 requires ignoring those, so nothing moved.
        //
        // A close tears every stream down without detach notifications, since it
        // clears the stream table rather than retiring stream by stream. Owners
        // must also observe on_state_change. So must an owner tracking admission:
        // reaching Established is a state change, not a credit change.
        //
        // Runs inside attach/retire and the packet frame loop: observe state,
        // update a gate, or schedule deferred work only. Do not synchronously
        // mutate the connection or its streams. In particular, defer closing
        // until the current packet has finished processing.
        void (*on_capacity_change)(void *owner, QuicConnection &connection) noexcept = nullptr;
        // Client role. A NewSessionTicket arrived; returning true transfers the
        // SSL_SESSION reference to the owner. Runs from inside the TLS stack:
        // store it and return, do not touch the connection.
        bool (*on_new_tls_session)(void *owner, QuicConnection &connection, SSL_SESSION *session) noexcept = nullptr;
        // Client role. A NEW_TOKEN frame arrived; the bytes are borrowed for
        // the call.
        void (*on_new_token)(void *owner, QuicConnection &connection, const std::uint8_t *token,
                             std::size_t token_len) noexcept = nullptr;
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
        // Ownership of the connection storage. Set: the storage is released by
        // on_destroy once the connection is detached and its last lease drops
        // (server connections, and clients that keep the lease model). Null:
        // the storage belongs to the caller, which destroys it after
        // wait_closed(); the endpoint only counts such a connection until it
        // detaches. Server admission requires a callback.
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
        // Server role. Consumed lazily by ensure_server_tls() once the first
        // Initial packet passes AEAD authentication (mirrors nginx
        // ngx_quic_init_connection, which runs after ngx_quic_decrypt). The
        // endpoint owns it and outlives the connection. Early data is accepted
        // iff server_tls->enable_early_data.
        const net::TlsServerParam *server_tls = nullptr;
    };

    // Every connection is hosted by an initialized endpoint that outlives it:
    // the loop, frame/crypto pools and receive-storage budget all come from
    // the endpoint, and QuicUdpEndpoint::shutdown() waits for every attached
    // connection to be destroyed before the endpoint closes. Construction
    // alone does not index the connection; the endpoint attaches it once its
    // connection IDs are registered.
    QuicConnection(QuicUdpEndpoint &endpoint, const Options &options) noexcept;
    ~QuicConnection();

    [[nodiscard]] QuicConnectionRole role() const noexcept { return options_.role; }
    [[nodiscard]] QuicConnectionState state() const noexcept { return state_; }
    // Server: whether 0-RTT is accepted. Client: whether connect() offered it.
    [[nodiscard]] bool early_data_enabled() const noexcept;
    [[nodiscard]] bool early_data_attempted() const noexcept { return early_data_attempted_; }
    [[nodiscard]] bool early_data_accepted() const noexcept { return early_data_accepted_; }
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
    [[nodiscard]] const QuicConnectionId &current_initial_destination_connection_id() const noexcept {
        return current_initial_destination_connection_id_;
    }
    [[nodiscard]] bool has_server_initial_source_connection_id() const noexcept {
        return has_server_initial_source_connection_id_;
    }
    [[nodiscard]] bool has_authenticated_server_packet() const noexcept { return has_authenticated_server_packet_; }
    [[nodiscard]] const QuicConnectionId &server_initial_source_connection_id() const noexcept {
        return server_initial_source_connection_id_;
    }
    [[nodiscard]] bool retried() const noexcept { return options_.has_retry_source_connection_id; }
    [[nodiscard]] bool retry_processed() const noexcept { return retry_processed_; }
    [[nodiscard]] const QuicConnectionId &retry_source_connection_id() const noexcept {
        return options_.retry_source_connection_id;
    }
    [[nodiscard]] QuicErrorCode close_error() const noexcept {
        return static_cast<QuicErrorCode>(close_info_.error_code);
    }
    [[nodiscard]] const QuicCloseInfo &close_info() const noexcept { return close_info_; }
    [[nodiscard]] common::IoErr connect_failure() const noexcept { return connect_failure_; }
    [[nodiscard]] QuicCloseSource close_source() const noexcept { return close_info_.source; }
    [[nodiscard]] bool closed() const noexcept { return state_ == QuicConnectionState::Closed; }
    [[nodiscard]] bool terminal_closing() const noexcept {
        return state_ == QuicConnectionState::Draining || state_ == QuicConnectionState::Closing ||
               state_ == QuicConnectionState::Closed;
    }
    [[nodiscard]] bool closing() const noexcept { return terminal_closing(); }
    [[nodiscard]] bool graceful_closing() const noexcept { return state_ == QuicConnectionState::GracefulClosing; }
    [[nodiscard]] QuicUdpEndpoint &endpoint() noexcept { return endpoint_; }
    [[nodiscard]] const QuicUdpEndpoint &endpoint() const noexcept { return endpoint_; }
    // Attached: indexed by the endpoint and serviced by its send scheduler.
    // Detached: closed and unindexed; nothing more is sent, but the object
    // stays alive -- and counted by the endpoint -- until its last lease drops.
    [[nodiscard]] bool attached_to_endpoint() const noexcept {
        return endpoint_attachment_ == EndpointAttachment::Attached;
    }
    [[nodiscard]] bool detached_from_endpoint() const noexcept {
        return endpoint_attachment_ == EndpointAttachment::Detached;
    }
    [[nodiscard]] std::uint32_t ref_count() const noexcept { return ref_count_; }
    [[nodiscard]] Lease lease() noexcept { return Lease(this); }

    common::IoResult<void> start_handshake() noexcept;
    common::IoResult<void> mark_established() noexcept;
    [[nodiscard]] async::Task<common::IoResult<void>>
    wait_established(std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    [[nodiscard]] async::Task<common::IoResult<void>>
    wait_confirmed(std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    // Client role only, exactly once, on the connection's loop, before attach.
    // Runs the whole client connect sequence synchronously -- 0-RTT memory,
    // Initial token and keys, TLS client SSL, first CRYPTO flight, attach --
    // and leaves the connection attached with its Initial queued for sending.
    // The caller then awaits wait_established()/wait_confirmed(). A
    // precondition failure (wrong role or state, off-loop) leaves the
    // connection untouched; any later failure leaves it unattached and Closed.
    // connect_error() names the phase either way.
    [[nodiscard]] common::IoResult<void> connect(const QuicClientConnectParams &params) noexcept;
    // Classifies a connect() or handshake-wait failure for a client. Reports
    // the phase connect() failed in, else what the handshake ran into: version
    // negotiation, transport parameters, TLS verification or alert, a peer
    // close, or the timeout.
    [[nodiscard]] QuicConnectError connect_error(common::IoErr error) const noexcept;
    // Seeds 0-RTT state from the transport parameters remembered alongside a
    // resumed session: the client sends early data against these limits until
    // the server's real parameters arrive. Client, Init state, before
    // start_handshake(). connect() applies it when early data is enabled and
    // both a session and remembered parameters are available.
    [[nodiscard]] common::IoResult<void> remember_peer_transport(const QuicTransportSettings &remembered) noexcept;
    // Resolves once the connection has detached from its endpoint (state
    // Closed); immediately for one that never attached. The caller keeps the
    // connection alive while parked -- it owns the storage, or holds a lease --
    // and destroying a connection with a waiter parked is asserted.
    [[nodiscard]] async::Task<void> wait_closed() noexcept;
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
    // What try_attach_local_stream would answer for a fresh stream right now:
    // None when one can be attached, Busy while the handshake has not reached
    // the point this early-data mode needs or the peer's stream credit is spent,
    // Canceled once the connection will never admit another local stream. A
    // caller waiting for room uses this to tell "retry later" from "never
    // again". Pure: unlike try_attach_local_stream it queues no STREAMS_BLOCKED
    // frame, so it cannot stand in for the real attempt.
    // Locally initiated streams of this type that could still be attached: the
    // peer's credit for the type minus what this side has already spent. Credit
    // only ever grows -- QUIC stream ids are monotonic, so retiring a stream
    // returns nothing here.
    [[nodiscard]] std::uint64_t available_local_stream_slots(QuicStreamType type) const noexcept;
    [[nodiscard]] common::IoErr local_stream_attach_status(
            QuicStreamType type,
            QuicStreamEarlyDataMode early_data_mode = QuicStreamEarlyDataMode::OneRttOnly) const noexcept;
    [[nodiscard]] bool accepts_new_local_stream(
            QuicStreamType type,
            QuicStreamEarlyDataMode early_data_mode = QuicStreamEarlyDataMode::OneRttOnly) const noexcept {
        return local_stream_attach_status(type, early_data_mode) == common::IoErr::None;
    }
    [[nodiscard]] common::IoResult<QuicStream *>
    try_attach_local_stream(QuicStream::Lease &&stream, QuicStreamType type,
                            QuicStreamEarlyDataMode early_data_mode = QuicStreamEarlyDataMode::OneRttOnly) noexcept;
    [[nodiscard]] common::IoResult<QuicStream *> get_or_create_peer_stream(std::uint64_t stream_id) noexcept;
    [[nodiscard]] common::IoResult<void> recv_stream_frame(const QuicStreamFrame &frame, mem::IoBuf data) noexcept;
    [[nodiscard]] common::IoResult<void> recv_reset_stream_frame(const QuicResetStreamFrame &frame) noexcept;
    [[nodiscard]] common::IoResult<void> recv_stop_sending_frame(const QuicStopSendingFrame &frame) noexcept;
    [[nodiscard]] common::IoResult<void> recv_max_stream_data_frame(const QuicMaxStreamDataFrame &frame) noexcept;
    [[nodiscard]] common::IoResult<void> recv_max_streams_frame(const QuicMaxStreamsFrame &frame) noexcept;
    [[nodiscard]] common::IoResult<void> recv_max_data_frame(const QuicMaxDataFrame &frame) noexcept;
    [[nodiscard]] common::IoResult<void> recv_streams_blocked_frame(const QuicStreamsBlockedFrame &frame) noexcept;
    [[nodiscard]] event::EventLoop &loop() const noexcept { return loop_; }
    [[nodiscard]] mem::IoBufNodePool &recv_extent_pool() noexcept { return loop_.io_buf_node_pool(); }
    [[nodiscard]] mem::IoBufStorageBudget &recv_storage_budget() noexcept { return recv_storage_budget_; }
    [[nodiscard]] std::size_t retained_recv_storage_capacity() const noexcept {
        return recv_storage_budget_.retained_capacity();
    }
    [[nodiscard]] std::size_t retained_recv_storage_high_water() const noexcept {
        return recv_storage_budget_.high_water();
    }
    [[nodiscard]] std::size_t retained_recv_storage_rejected_count() const noexcept {
        return recv_storage_budget_.rejected_count();
    }
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
    [[nodiscard]] bool key_phase() const noexcept { return crypto_.epoch().phase; }
    [[nodiscard]] bool next_keys_ready() noexcept { return crypto_.next_application_keys_ready(); }
    [[nodiscard]] bool handshake_confirmed() const noexcept { return crypto_.epoch().handshake_confirmed; }
    void confirm_handshake() noexcept;
    [[nodiscard]] common::IoResult<void> prepare_application_packet_encryption() noexcept;
    void on_application_packet_encrypted(std::uint64_t packet_number) noexcept;
    void on_application_packet_acked(std::uint64_t largest_acked, std::chrono::steady_clock::time_point now) noexcept;
    [[nodiscard]] common::IoResult<void> apply_peer_key_update(std::uint64_t packet_number) noexcept;
    void on_application_packet_authenticated(QuicReadKeyEpoch epoch, std::uint64_t packet_number) noexcept;
    [[nodiscard]] bool record_application_authentication_failure() noexcept;
    void arm_key_update_discard_timer() noexcept;
    void cancel_key_update_discard_timer() noexcept;
    void on_packet_processed() noexcept;
    void on_ack_eliciting_packet_sent() noexcept;
    [[nodiscard]] std::chrono::milliseconds effective_idle_timeout() const noexcept;
    // How long the connection stays silent before it sends a keepalive PING.
    // Zero when keepalive is off, which is the default: RFC 9000 10.1.2 leaves
    // deferring the idle timeout to the application, and warns that doing it for
    // a connection unlikely to be used again wastes both endpoints' resources.
    [[nodiscard]] std::chrono::milliseconds keepalive_delay() const noexcept;
    [[nodiscard]] bool idle_timer_armed() const noexcept { return idle_timer_entry_.is_in_heap(); }
    [[nodiscard]] bool close_timer_armed() const noexcept { return close_timer_entry_.is_in_heap(); }
    [[nodiscard]] bool keepalive_timer_armed() const noexcept { return keepalive_timer_entry_.is_in_heap(); }
    [[nodiscard]] bool ack_timer_armed() const noexcept { return ack_timer_entry_.is_in_heap(); }
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
    common::IoResult<void> reinit_initial_crypto(const QuicConnectionId &destination_cid) noexcept;
    [[nodiscard]] common::IoResult<void>
    adopt_server_initial_source_connection_id(const QuicConnectionId &cid) noexcept;
    void note_authenticated_server_packet() noexcept { has_authenticated_server_packet_ = true; }
    [[nodiscard]] common::IoResult<bool> handle_retry(const QuicPacketHeader &packet,
                                                      const QuicReceivedDatagram &datagram) noexcept;
    [[nodiscard]] bool handle_version_negotiation(const QuicPacketHeader &packet) noexcept;
    [[nodiscard]] const mem::IoBuf &initial_token() const noexcept { return initial_token_; }
    [[nodiscard]] common::IoResult<void> set_initial_token(const std::uint8_t *token, std::size_t token_len) noexcept;
    [[nodiscard]] common::IoResult<void> recv_new_token_frame(const QuicInputFrame &frame) noexcept;
    [[nodiscard]] bool on_new_tls_session(SSL_SESSION *session) noexcept;
    void fail_client_connect(common::IoErr error) noexcept;
    // Lazily create the server SSL object the first time an Initial packet
    // is authenticated. Idempotent; no-op when no TLS parameters are configured
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
    [[nodiscard]] common::IoResult<void> on_path_validated(QuicPath &path, QuicTime now) noexcept;
    [[nodiscard]] common::IoResult<void> on_early_data_accepted() noexcept;
    void on_early_data_rejected() noexcept;
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
    // Registration in the owning endpoint's connection list. The hook is the
    // list membership; the lease keeps this connection alive while indexed,
    // until detach moves it out to order its release after unindexing.
    common::IntrusiveListHook endpoint_link_{};
    Lease endpoint_lease_{};
    QuicConnectionIdIndex original_dcid_index{};
    // Membership in the endpoint send scheduler's ready ring.
    common::IntrusiveListHook send_queue_hook_{};

private:
    enum class EndpointAttachment : std::uint8_t {
        Unattached,
        Attached,
        Detached,
    };

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
    // Whether this mode may attach before the handshake completes: 0-RTT keys
    // are installed and this connection actually attempted early data.
    [[nodiscard]] bool early_attach_ready(QuicStreamEarlyDataMode early_data_mode) const noexcept;
    [[nodiscard]] bool is_gone_peer_stream(std::uint64_t stream_id) const noexcept;
    // RFC 9000 §4.6: a peer-initiated stream whose sequence (id >> 2) reaches or
    // exceeds the advertised max_streams has exceeded the limit advertised via
    // MAX_STREAMS — a STREAM_LIMIT_ERROR peer violation. This is distinct from
    // the concurrent-active-stream window (can_accept_peer_stream's second
    // check), which is a non-fatal flow-control gate and must NOT close the
    // connection.
    [[nodiscard]] bool peer_stream_exceeds_advertised_limit(std::uint64_t stream_id) const noexcept;
    [[nodiscard]] common::IoResult<QuicStream *>
    attach_stream(QuicStream::Lease &&lease, std::uint64_t stream_id, QuicStreamRecvQueue::Options recv_options,
                  bool local_initiated,
                  QuicStreamEarlyDataMode early_data_mode = QuicStreamEarlyDataMode::OneRttOnly) noexcept;
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
    void install_remote_stateless_reset_token(QuicRemoteConnectionIdSlot &slot,
                                              const std::uint8_t token[kStatelessResetTokenLength]) noexcept;
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
    // The single writer of state_. Re-evaluates handshake waiters, whose
    // resumes are deferred. Set close_info_ first. Each transition entry point
    // must finish its bookkeeping before dispatch_state_change(), and must not
    // continue that bookkeeping after the synchronous observer has run.
    void publish_state(QuicConnectionState next) noexcept;
    void dispatch_state_change() noexcept;
    void dispatch_capacity_change() noexcept;
    void enter_graceful_closing(QuicCloseInfo info, std::chrono::milliseconds grace) noexcept;
    void enter_closing(QuicCloseInfo info, bool immediate = false) noexcept;
    void enter_draining(QuicCloseInfo info) noexcept;
    void enter_closed() noexcept;
    void maybe_finish_graceful_close() noexcept;
    void assert_loop_affinity() const noexcept;
    [[nodiscard]] event::EventLoop *active_timer_loop() const noexcept;
    void arm_ack_timer(std::chrono::steady_clock::time_point deadline) noexcept;
    void cancel_ack_timer() noexcept;
    void arm_pacing_timer(std::chrono::steady_clock::time_point deadline) noexcept;
    void cancel_pacing_timer() noexcept;
    void cancel_all_timers_quiesced() noexcept;
    [[nodiscard]] bool has_pending_send_work() const noexcept;
    [[nodiscard]] bool has_pacing_exempt_send_work() const noexcept;
    [[nodiscard]] bool reserve_peer_data(std::uint64_t bytes) noexcept;
    [[nodiscard]] std::uint64_t initial_stream_send_limit(std::uint64_t stream_id) const noexcept;
    void wait_for_peer_data(QuicStream::WriteAwaiter &awaiter) noexcept;
    void cancel_peer_data_wait(QuicStream::WriteAwaiter &awaiter) noexcept;
    void notify_peer_data_waiters(common::IoErr result = common::IoErr::None) noexcept;
    void reset_after_retry() noexcept;
    [[nodiscard]] common::IoResult<void> start_preferred_path_validation() noexcept;
    void attach_to_endpoint() noexcept;
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
    static void on_ack_timer(QuicConnection *connection) noexcept;
    static void on_pacing_timer(QuicConnection *connection) noexcept;

    friend class QuicPathManager;
    friend class QuicSendScheduler;

    Options options_{};
    QuicUdpEndpoint &endpoint_;
    // The endpoint's loop, cached off the timer paths.
    event::EventLoop &loop_;
    QuicConnectionId current_initial_destination_connection_id_{};
    QuicConnectionId server_initial_source_connection_id_{};
    QuicConnectionState state_ = QuicConnectionState::Init;
    std::uint64_t next_local_bidi_stream_id_ = 0;
    std::uint64_t next_local_uni_stream_id_ = 0;
    PeerStreamLimitWindow peer_bidi_streams_{};
    PeerStreamLimitWindow peer_uni_streams_{};
    LocalStreamBlockedState local_bidi_streams_blocked_{};
    LocalStreamBlockedState local_uni_streams_blocked_{};
    mem::IoBufStorageBudget recv_storage_budget_{};
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
    event::EventLoop::TimerEntry ack_timer_entry_{};
    event::EventLoop::TimerEntry pacing_timer_entry_{};
    QuicPacerState pacer_{};
    QuicCryptoState crypto_{};
    mem::IoBuf initial_token_{};
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
    std::uint64_t preferred_path_seqnum_ = kQuicNoPathSeqnum;
    std::uint64_t recv_data_consumed_ = 0;
    std::uint64_t recv_data_limit_ = 0;
    std::uint64_t peer_max_data_ = 0;
    std::uint64_t peer_data_reserved_ = 0;
    std::uint64_t last_data_blocked_limit_ = 0;
    QuicHandshakeGate handshake_gate_{*this};
    // Connection-window send waiters, as a hook ring anchored here. The queued
    // nodes are QuicStream::WriteAwaiter hooks; IntrusiveList<T, Offset> cannot
    // be named in this header because WriteAwaiter is defined in QuicStream.cpp,
    // so the ring is driven with IntrusiveListHook primitives instead.
    common::IntrusiveListHook peer_data_wait_anchor_{};
    bool data_blocked_reported_ = false;
    bool idle_send_timer_set_ = false;
    bool has_server_initial_source_connection_id_ = false;
    bool has_authenticated_server_packet_ = false;
    bool retry_processed_ = false;
    // Client 0-RTT: what connect() offered, and the peer transport remembered
    // with the resumed session while early data is in flight.
    bool early_data_enabled_ = false;
    bool early_data_attempted_ = false;
    bool early_data_accepted_ = false;
    QuicTransportSettings remembered_peer_transport_{};
    std::uint64_t early_next_local_bidi_stream_id_ = 0;
    std::uint64_t early_next_local_uni_stream_id_ = 0;
    std::uint64_t early_peer_data_reserved_ = 0;
    bool state_dispatch_running_ = false;
    bool state_dispatch_again_ = false;
    bool capacity_dispatch_running_ = false;
    bool capacity_dispatch_again_ = false;
    EndpointAttachment endpoint_attachment_ = EndpointAttachment::Unattached;
    std::uint32_t ref_count_ = 1;
    void *destroy_owner_ = nullptr;
    DestroyCallback on_destroy_ = nullptr;
    // close_info_ is interpreted by state_: in GracefulClosing it is the staged
    // final close, in Closing it is the sent CONNECTION_CLOSE, and in Draining it
    // is the peer/stateless-reset reason. last_cc_msec_ rate-limits close-frame
    // requeueing in Closing.
    QuicCloseInfo close_info_{};
    common::IoErr connect_failure_ = common::IoErr::None;
    // Where connect() itself failed; Handshake once it handed off to the wire.
    QuicConnectPhase connect_phase_ = QuicConnectPhase::Handshake;
    std::chrono::milliseconds last_cc_msec_{0};
    // Coroutines parked in wait_closed(): counted from attach to detach.
    async::LocalWaitGroup closed_{};

    friend class QuicStream;
    friend class QuicStream::WriteAwaiter;
    friend class QuicUdpEndpoint;
};

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_CONNECTION_H
