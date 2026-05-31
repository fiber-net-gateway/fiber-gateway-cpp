#ifndef FIBER_QUIC_QUIC_CONNECTION_H
#define FIBER_QUIC_QUIC_CONNECTION_H

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

#include "../common/IntrusiveList.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../net/SocketAddress.h"

namespace fiber::quic {

inline constexpr std::size_t kMaxConnectionIdLength = 20;
inline constexpr std::size_t kQuicPacketNumberSpaceCount = 3;
inline constexpr std::size_t kQuicMaxAckRanges = 32;

enum class QuicEncryptionLevel : std::uint8_t;
struct QuicFrame;

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

enum class QuicStreamType : std::uint8_t {
    Bidirectional,
    Unidirectional,
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

struct QuicAckRange {
    std::uint64_t gap = 0;
    std::uint64_t range = 0;
};

struct QuicFrameQueue {
    [[nodiscard]] bool empty() const noexcept { return head_ == nullptr; }
    [[nodiscard]] QuicFrame *front() noexcept;
    [[nodiscard]] const QuicFrame *front() const noexcept;
    [[nodiscard]] QuicFrame *back() noexcept;
    [[nodiscard]] const QuicFrame *back() const noexcept;

    void push_back(QuicFrame &frame) noexcept;
    void erase(QuicFrame &frame) noexcept;

private:
    [[nodiscard]] static QuicFrame *owner_from_hook(common::IntrusiveListHook *hook) noexcept;
    [[nodiscard]] static const QuicFrame *owner_from_hook(const common::IntrusiveListHook *hook) noexcept;

    common::IntrusiveListHook *head_ = nullptr;
    common::IntrusiveListHook *tail_ = nullptr;
};

struct QuicPacketNumberSpace {
    QuicPacketNumberSpace() noexcept;

    void reset(QuicEncryptionLevel space_level) noexcept;
    void record_received_packet_number(std::uint64_t packet_number) noexcept;
    void record_acked_packet_number(std::uint64_t packet_number) noexcept;

    QuicEncryptionLevel level;

    std::uint64_t crypto_sent = 0;

    std::uint64_t next_packet_number = 0;
    std::uint64_t largest_acked_packet_number = 0;
    std::uint64_t largest_received_packet_number = 0;

    QuicFrameQueue pending_frames{};
    QuicFrameQueue sending_frames{};
    QuicFrameQueue sent_frames{};

    std::uint64_t pending_ack = 0;
    std::uint64_t largest_range = 0;
    std::uint64_t first_range = 0;
    std::chrono::milliseconds largest_received_time{0};
    std::chrono::milliseconds ack_delay_start{0};
    std::uint32_t ack_range_count = 0;
    std::array<QuicAckRange, kQuicMaxAckRanges> ack_ranges{};
    bool send_ack = false;
};

struct QuicPacketNumberSpaceSnapshot {
    std::uint64_t next_packet_number = 0;
};

class QuicConnection : public common::NonCopyable, public common::NonMovable {
public:
    struct Options {
        QuicConnectionRole role = QuicConnectionRole::Server;
        net::SocketAddress local_addr{};
        net::SocketAddress remote_addr{};
        QuicConnectionId local_connection_id{};
        QuicConnectionId remote_connection_id{};
        std::chrono::milliseconds idle_timeout = std::chrono::seconds(30);
        std::uint64_t max_peer_bidirectional_streams = 128;
        std::uint64_t max_peer_unidirectional_streams = 32;
        std::uint64_t max_local_bidirectional_streams = 128;
        std::uint64_t max_local_unidirectional_streams = 32;
    };

    explicit QuicConnection(const Options &options) noexcept;
    ~QuicConnection() = default;

    [[nodiscard]] QuicConnectionRole role() const noexcept { return options_.role; }
    [[nodiscard]] QuicConnectionState state() const noexcept { return state_; }
    [[nodiscard]] const net::SocketAddress &local_addr() const noexcept { return options_.local_addr; }
    [[nodiscard]] const net::SocketAddress &remote_addr() const noexcept { return options_.remote_addr; }
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

    [[nodiscard]] bool is_local_stream(std::uint64_t stream_id) const noexcept;
    [[nodiscard]] bool is_peer_stream(std::uint64_t stream_id) const noexcept { return !is_local_stream(stream_id); }
    [[nodiscard]] static bool is_bidirectional_stream(std::uint64_t stream_id) noexcept;
    [[nodiscard]] static bool is_unidirectional_stream(std::uint64_t stream_id) noexcept;
    [[nodiscard]] static QuicStreamType stream_type(std::uint64_t stream_id) noexcept;

    [[nodiscard]] QuicPacketNumberSpace &packet_number_space(QuicEncryptionLevel level) noexcept;
    [[nodiscard]] const QuicPacketNumberSpace &packet_number_space(QuicEncryptionLevel level) const noexcept;
    [[nodiscard]] static std::size_t packet_number_space_index(QuicEncryptionLevel level) noexcept;

private:
    [[nodiscard]] std::uint8_t local_initiator_bit() const noexcept;
    [[nodiscard]] std::uint64_t local_stream_limit(QuicStreamType type) const noexcept;
    [[nodiscard]] std::uint64_t peer_stream_limit(QuicStreamType type) const noexcept;

    Options options_{};
    QuicConnectionState state_ = QuicConnectionState::Init;
    QuicErrorCode close_error_ = QuicErrorCode::NoError;
    std::uint64_t next_local_bidi_stream_id_ = 0;
    std::uint64_t next_local_uni_stream_id_ = 0;
    std::uint64_t largest_peer_bidi_sequence_ = 0;
    std::uint64_t largest_peer_uni_sequence_ = 0;
    std::array<QuicPacketNumberSpace, kQuicPacketNumberSpaceCount> packet_number_spaces_{};
};

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_CONNECTION_H
