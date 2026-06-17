#ifndef FIBER_QUIC_QUIC_CONNECTION_H
#define FIBER_QUIC_QUIC_CONNECTION_H

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

#include <openssl/aead.h>
#include <openssl/aes.h>

#include "../common/IntrusiveList.h"
#include "../common/IntrusiveRbTree.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../common/mem/IoBufChain.h"
#include "../net/SocketAddress.h"
#include "QuicCongestion.h"
#include "QuicFrame.h"
#include "QuicPacketNumberSpace.h"
#include "QuicStream.h"
#include "QuicStreamTable.h"
#include "QuicTlsSession.h"

namespace fiber::quic {

struct QuicTransportParams;

inline constexpr std::size_t kQuicConnectionIdLength = kMaxConnectionIdLength;
inline constexpr std::size_t kQuicMaxCryptoBuffered = 65536;
inline constexpr std::size_t kQuicMaxCryptoBufferedSegments = 16;
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
inline constexpr std::size_t kQuicMaxPaths = 3;
inline constexpr std::size_t kQuicPathRetries = 3;
inline constexpr std::size_t kQuicMaxUdpPayloadSize = 65527;
inline constexpr std::size_t kQuicDefaultStreamBufferSize = 65536;
inline constexpr std::uint64_t kQuicDefaultMaxBidirectionalStreams = 128;
inline constexpr std::uint64_t kQuicDefaultMaxUnidirectionalStreams = 32;
inline constexpr std::uint64_t kQuicDefaultInitialMaxData =
        (kQuicDefaultMaxBidirectionalStreams + kQuicDefaultMaxUnidirectionalStreams) * kQuicDefaultStreamBufferSize;
inline constexpr std::size_t kQuicRetiredStreamRecordCount = 1024;

enum class QuicConnectionRole : std::uint8_t {
    Client,
    Server,
};

enum class QuicConnectionState : std::uint8_t {
    Init,
    Handshaking,
    Established,
    Draining,
    Closing,
    Closed,
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

struct QuicConnectionId {
    std::array<std::uint8_t, kMaxConnectionIdLength> bytes{};
    std::uint8_t length = 0;

    [[nodiscard]] bool empty() const noexcept { return length == 0; }
    [[nodiscard]] const std::uint8_t *data() const noexcept { return bytes.data(); }
    [[nodiscard]] std::size_t size() const noexcept { return length; }

    static common::IoResult<QuicConnectionId> from_bytes(const std::uint8_t *data, std::size_t len) noexcept;
};

struct QuicCryptoBufferedSegment {
    std::unique_ptr<std::uint8_t[]> data{};
    std::uint64_t offset = 0;
    std::size_t len = 0;
    bool used = false;
};

struct QuicCryptoRecvBuffer {
    std::uint64_t next_offset = 0;
    std::array<QuicCryptoBufferedSegment, kQuicMaxCryptoBufferedSegments> segments{};
};

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
    std::uint64_t active_connection_id_limit = 2;
    bool disable_active_migration = false;
};

struct QuicPeerTransportState {
    QuicTransportSettings params{};
    bool received = false;
};

struct QuicRetiredStreamRecord {
    std::uint64_t stream_id = 0;
    std::uint64_t final_size = 0;
    bool used = false;
    bool has_final_size = false;
    bool reset_received = false;
};

class QuicConnection;

struct QuicNewStreamContext {
    std::uint64_t stream_id = 0;
    QuicConnection &connection;
    mem::IoBufNodePool &recv_extent_pool;
};

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
    bool initial_ready = false;
};

enum class QuicPathTag : std::uint8_t {
    Probe,
    Active,
    Backup,
};

enum class QuicPathState : std::uint8_t {
    Idle,
    Validating,
    WaitingMtuProbe,
    MtuDiscovery,
};

struct QuicPath {
    net::SocketAddress remote{};
    net::SocketAddress local{};
    QuicConnectionId remote_connection_id{};
    std::uint64_t remote_connection_id_sequence = 0;
    QuicPathState state = QuicPathState::Idle;
    QuicTime expires{0};
    std::uint32_t tries = 0;
    QuicPathTag tag = QuicPathTag::Probe;
    std::size_t mtu = kQuicCongestionMinInitialSize;
    std::size_t mtud = 0;
    std::size_t max_mtu = 0;
    std::uint64_t sent = 0;
    std::uint64_t received = 0;
    std::uint8_t challenge[2][8]{};
    std::uint64_t seqnum = 0;
    std::uint64_t mtu_packet_numbers[kQuicPathRetries]{};
    bool allocated = false;
    bool validated = false;
    bool mtu_unvalidated = false;
    bool used = false;
};

class QuicConnection : public common::NonCopyable, public common::NonMovable {
public:
    struct Ops {
        QuicStream::Lease (*on_new_stream)(void *owner, const QuicNewStreamContext &ctx) noexcept = nullptr;
    };

    struct EndpointIndex {
        QuicConnection *connection = nullptr;
        common::IntrusiveListHook link{};
    };

    struct ConnectionIdIndex {
        QuicConnection *connection = nullptr;
        QuicConnectionId cid_key{};
        std::uint64_t cid_hash = 0;
        common::IntrusiveRbTreeHook cid_hook{};
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
        QuicConnectionId local_connection_id{};
        QuicConnectionId remote_connection_id{};
        QuicTransportSettings transport{};
        std::uint64_t max_peer_bidirectional_streams = kQuicDefaultMaxBidirectionalStreams;
        std::uint64_t max_peer_unidirectional_streams = kQuicDefaultMaxUnidirectionalStreams;
        std::uint64_t max_local_bidirectional_streams = kQuicDefaultMaxBidirectionalStreams;
        std::uint64_t max_local_unidirectional_streams = kQuicDefaultMaxUnidirectionalStreams;
        QuicOutputFramePool *output_frame_pool = nullptr;
        void *owner = nullptr;
        Ops ops{};
    };

    explicit QuicConnection(const Options &options) noexcept;
    ~QuicConnection() = default;

    [[nodiscard]] QuicConnectionRole role() const noexcept { return options_.role; }
    [[nodiscard]] QuicConnectionState state() const noexcept { return state_; }
    [[nodiscard]] const net::SocketAddress &local_addr() const noexcept { return options_.local_addr; }
    [[nodiscard]] const net::SocketAddress &remote_addr() const noexcept { return options_.remote_addr; }
    [[nodiscard]] const QuicConnectionId &original_destination_connection_id() const noexcept {
        return options_.original_destination_connection_id;
    }
    [[nodiscard]] const QuicConnectionId &local_connection_id() const noexcept { return options_.local_connection_id; }
    [[nodiscard]] const QuicConnectionId &remote_connection_id() const noexcept {
        return options_.remote_connection_id;
    }
    [[nodiscard]] QuicErrorCode close_error() const noexcept { return close_error_; }
    [[nodiscard]] bool closed() const noexcept { return state_ == QuicConnectionState::Closed; }
    [[nodiscard]] bool closing() const noexcept {
        return state_ == QuicConnectionState::Draining || state_ == QuicConnectionState::Closing ||
               state_ == QuicConnectionState::Closed;
    }

    common::IoResult<void> start_handshake() noexcept;
    common::IoResult<void> mark_established() noexcept;
    void begin_draining(QuicErrorCode error = QuicErrorCode::NoError) noexcept;
    void close(QuicErrorCode error = QuicErrorCode::NoError) noexcept;
    void mark_closed() noexcept;

    [[nodiscard]] common::IoResult<std::uint64_t> next_local_stream_id(QuicStreamType type) noexcept;
    [[nodiscard]] bool can_accept_peer_stream(std::uint64_t stream_id) const noexcept;
    common::IoResult<void> record_peer_stream_id(std::uint64_t stream_id) noexcept;
    [[nodiscard]] QuicStream *find_stream(std::uint64_t stream_id) noexcept;
    [[nodiscard]] const QuicStream *find_stream(std::uint64_t stream_id) const noexcept;
    [[nodiscard]] std::size_t active_stream_count() const noexcept { return streams_.size(); }
    [[nodiscard]] common::IoResult<QuicStream *> get_or_create_peer_stream(std::uint64_t stream_id) noexcept;
    [[nodiscard]] common::IoResult<void> recv_stream_frame(const QuicStreamFrame &frame, QuicSlice data) noexcept;
    [[nodiscard]] common::IoResult<void> recv_reset_stream_frame(const QuicResetStreamFrame &frame) noexcept;
    [[nodiscard]] common::IoResult<void> recv_stop_sending_frame(const QuicStopSendingFrame &frame) noexcept;
    [[nodiscard]] common::IoResult<void> recv_max_stream_data_frame(const QuicMaxStreamDataFrame &frame) noexcept;
    [[nodiscard]] common::IoResult<void> recv_max_data_frame(const QuicMaxDataFrame &frame) noexcept;
    [[nodiscard]] mem::IoBufNodePool &recv_extent_pool() noexcept { return recv_extent_pool_; }
    void release_stream_app(QuicStream &stream) noexcept;
    [[nodiscard]] std::uint64_t recv_data_consumed() const noexcept { return recv_data_consumed_; }
    [[nodiscard]] std::uint64_t recv_data_limit() const noexcept { return recv_data_limit_; }
    [[nodiscard]] std::uint64_t peer_max_data() const noexcept { return peer_max_data_; }

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
    [[nodiscard]] QuicCongestionState &congestion() noexcept { return congestion_; }
    [[nodiscard]] const QuicCongestionState &congestion() const noexcept { return congestion_; }
    [[nodiscard]] QuicRttState &rtt() noexcept { return rtt_; }
    [[nodiscard]] const QuicRttState &rtt() const noexcept { return rtt_; }
    [[nodiscard]] std::uint64_t reset_packet_number() const noexcept { return reset_packet_number_; }
    void reset_congestion_for_path(QuicTime now) noexcept;
    [[nodiscard]] QuicPath *active_path() noexcept { return active_path_; }
    [[nodiscard]] const QuicPath *active_path() const noexcept { return active_path_; }
    [[nodiscard]] std::size_t path_count() const noexcept;
    [[nodiscard]] QuicPath *find_path(const net::SocketAddress &remote, const net::SocketAddress &local) noexcept;
    [[nodiscard]] const QuicPath *find_path(const net::SocketAddress &remote,
                                            const net::SocketAddress &local) const noexcept;
    [[nodiscard]] QuicPath *find_path(QuicPathTag tag) noexcept;
    [[nodiscard]] const QuicPath *find_path(QuicPathTag tag) const noexcept;
    [[nodiscard]] QuicPath *create_path(const net::SocketAddress &remote, const net::SocketAddress &local,
                                        const QuicConnectionId &remote_connection_id, QuicPathTag tag) noexcept;
    void free_path(QuicPath &path) noexcept;
    [[nodiscard]] bool set_active_path(QuicPath &path) noexcept;
    void record_path_received(QuicPath &path, std::size_t len) noexcept;
    void record_path_sent(QuicPath &path, std::size_t len) noexcept;
    [[nodiscard]] static std::size_t path_send_limit(const QuicPath &path, std::size_t size) noexcept;
    [[nodiscard]] QuicTlsSession &tls() noexcept { return tls_; }
    [[nodiscard]] const QuicTlsSession &tls() const noexcept { return tls_; }
    common::IoResult<void> init_initial_crypto(const QuicConnectionId &original_dcid) noexcept;
    common::IoResult<void> apply_peer_transport_params(const QuicTransportParams &params) noexcept;
    [[nodiscard]] const QuicTransportSettings &local_transport() const noexcept { return options_.transport; }
    [[nodiscard]] const QuicPeerTransportState &peer_transport() const noexcept { return peer_transport_; }
    [[nodiscard]] bool peer_transport_params_received() const noexcept { return peer_transport_.received; }
    [[nodiscard]] QuicCryptoRecvBuffer &crypto_recv_buffer(QuicEncryptionLevel level) noexcept;
    [[nodiscard]] const QuicCryptoRecvBuffer &crypto_recv_buffer(QuicEncryptionLevel level) const noexcept;

    EndpointIndex endpoint_index{};
    ConnectionIdIndex original_dcid_index{};
    ConnectionIdIndex local_cid_index{};
    SendQueueEntry send_queue_entry{};

private:
    [[nodiscard]] std::uint8_t local_initiator_bit() const noexcept;
    [[nodiscard]] std::uint64_t local_stream_limit(QuicStreamType type) const noexcept;
    [[nodiscard]] std::uint64_t peer_stream_limit(QuicStreamType type) const noexcept;
    [[nodiscard]] const QuicRetiredStreamRecord *find_retired_stream(std::uint64_t stream_id) const noexcept;
    void retire_stream(QuicStream &stream) noexcept;
    void record_retired_stream(const QuicStream &stream) noexcept;
    void try_release_stream(QuicStream &stream) noexcept;
    [[nodiscard]] common::IoResult<void> queue_reset_stream_frame(std::uint64_t stream_id, std::uint64_t error_code,
                                                                  std::uint64_t final_size) noexcept;
    [[nodiscard]] common::IoResult<void> queue_stop_sending_frame(std::uint64_t stream_id,
                                                                  std::uint64_t error_code) noexcept;
    [[nodiscard]] common::IoResult<void> queue_max_stream_data_frame(std::uint64_t stream_id,
                                                                     std::uint64_t limit) noexcept;
    [[nodiscard]] common::IoResult<void> queue_max_data_frame(std::uint64_t limit) noexcept;
    [[nodiscard]] common::IoResult<void> check_recv_data_delta(std::uint64_t delta) const noexcept;
    void commit_recv_data_delta(std::uint64_t delta) noexcept;
    void on_stream_data_consumed(std::uint64_t bytes) noexcept;
    [[nodiscard]] static common::IoResult<void> check_retired_stream_frame(const QuicRetiredStreamRecord &retired,
                                                                           std::uint64_t offset, QuicSlice data,
                                                                           bool fin) noexcept;
    [[nodiscard]] static common::IoResult<void> check_retired_reset_stream_frame(const QuicRetiredStreamRecord &retired,
                                                                                 std::uint64_t final_size) noexcept;

    Options options_{};
    QuicConnectionState state_ = QuicConnectionState::Init;
    QuicErrorCode close_error_ = QuicErrorCode::NoError;
    std::uint64_t next_local_bidi_stream_id_ = 0;
    std::uint64_t next_local_uni_stream_id_ = 0;
    std::uint64_t largest_peer_bidi_sequence_ = 0;
    std::uint64_t largest_peer_uni_sequence_ = 0;
    QuicOutputFramePool output_frame_pool_{};
    std::array<QuicPacketNumberSpace, kQuicPacketNumberSpaceCount> packet_number_spaces_{};
    QuicCongestionState congestion_{};
    QuicRttState rtt_{};
    std::uint64_t reset_packet_number_ = 0;
    QuicCryptoState crypto_{};
    QuicPeerTransportState peer_transport_{};
    mem::IoBufNodePool recv_extent_pool_{};
    QuicStreamTable streams_{};
    std::array<QuicRetiredStreamRecord, kQuicRetiredStreamRecordCount> retired_streams_{};
    std::uint64_t next_retired_stream_slot_ = 0;
    std::array<QuicCryptoRecvBuffer, kQuicPacketNumberSpaceCount> crypto_recv_buffers_{};
    QuicTlsSession tls_{};
    std::array<QuicPath, kQuicMaxPaths> paths_{};
    QuicPath *active_path_ = nullptr;
    std::uint64_t next_path_seqnum_ = 0;
    std::uint64_t recv_data_consumed_ = 0;
    std::uint64_t recv_data_released_ = 0;
    std::uint64_t recv_data_limit_ = 0;
    std::uint64_t peer_max_data_ = 0;

    friend class QuicStream;
};

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_CONNECTION_H
