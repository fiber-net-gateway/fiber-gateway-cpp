#ifndef FIBER_QUIC_QUIC_PROTOCOL_H
#define FIBER_QUIC_QUIC_PROTOCOL_H

#include <cstddef>
#include <cstdint>

#include "../common/IntrusiveList.h"
#include "QuicConnection.h"

namespace fiber::quic {

inline constexpr std::uint32_t kQuicVersion1 = 0x00000001;
inline constexpr std::size_t kMinInitialDatagramSize = 1200;
inline constexpr std::size_t kAeadTagLength = 16;
inline constexpr std::size_t kHeaderProtectionMaskLength = 5;
inline constexpr std::size_t kHeaderProtectionSampleLength = 16;
inline constexpr std::size_t kStatelessResetTokenLength = 16;
inline constexpr std::uint64_t kMaxVarint = (1ULL << 62U) - 1U;
inline constexpr std::uint64_t kUnsetPacketNumber = UINT64_MAX;

inline constexpr std::uint8_t kPacketFlagLong = 0x80;
inline constexpr std::uint8_t kPacketFlagFixed = 0x40;
inline constexpr std::uint8_t kPacketFlagLongTypeMask = 0x30;
inline constexpr std::uint8_t kPacketFlagKeyPhase = 0x04;
inline constexpr std::uint8_t kPacketFlagPnLengthMask = 0x03;

inline constexpr std::uint8_t kLongPacketTypeInitial = 0x00;
inline constexpr std::uint8_t kLongPacketTypeZeroRtt = 0x10;
inline constexpr std::uint8_t kLongPacketTypeHandshake = 0x20;
inline constexpr std::uint8_t kLongPacketTypeRetry = 0x30;

enum class QuicPacketType : std::uint8_t {
    Initial,
    ZeroRtt,
    Handshake,
    Retry,
    Short,
    VersionNegotiation,
};

enum class QuicEncryptionLevel : std::uint8_t {
    Initial = 0,
    EarlyData = 1,
    Handshake = 2,
    Application = 3,
};

enum class QuicFrameType : std::uint64_t {
    Padding = 0x00,
    Ping = 0x01,
    Ack = 0x02,
    AckEcn = 0x03,
    ResetStream = 0x04,
    StopSending = 0x05,
    Crypto = 0x06,
    NewToken = 0x07,
    Stream = 0x08,
    Stream1 = 0x09,
    Stream2 = 0x0A,
    Stream3 = 0x0B,
    Stream4 = 0x0C,
    Stream5 = 0x0D,
    Stream6 = 0x0E,
    Stream7 = 0x0F,
    MaxData = 0x10,
    MaxStreamData = 0x11,
    MaxStreamsBidi = 0x12,
    MaxStreamsUni = 0x13,
    DataBlocked = 0x14,
    StreamDataBlocked = 0x15,
    StreamsBlockedBidi = 0x16,
    StreamsBlockedUni = 0x17,
    NewConnectionId = 0x18,
    RetireConnectionId = 0x19,
    PathChallenge = 0x1A,
    PathResponse = 0x1B,
    ConnectionClose = 0x1C,
    ConnectionCloseApp = 0x1D,
    HandshakeDone = 0x1E,
};

inline constexpr std::uint64_t kLastFrameType = static_cast<std::uint64_t>(QuicFrameType::HandshakeDone);

struct QuicSlice {
    const std::uint8_t *data = nullptr;
    std::size_t len = 0;

    [[nodiscard]] bool empty() const noexcept { return len == 0; }
};

struct QuicPacketHeader {
    const std::uint8_t *packet_data = nullptr;
    std::size_t packet_len = 0;

    std::uint8_t protected_flags = 0;
    std::uint8_t flags = 0;
    bool long_header = false;
    QuicPacketType type = QuicPacketType::Short;
    QuicEncryptionLevel level = QuicEncryptionLevel::Application;

    std::uint32_t version = 0;
    QuicConnectionId dcid{};
    QuicConnectionId scid{};
    QuicSlice token{};

    std::uint64_t length = 0;
    const std::uint8_t *protected_pn = nullptr;
    const std::uint8_t *ciphertext = nullptr;
    std::size_t ciphertext_len = 0;

    std::uint8_t pn_len = 0;
    std::uint32_t truncated_pn = 0;
    std::uint64_t packet_number = 0;
};

struct QuicPaddingFrame {
    std::uint64_t length = 1;
};

struct QuicAckFrame {
    std::uint64_t largest = 0;
    std::uint64_t delay = 0;
    std::uint64_t range_count = 0;
    std::uint64_t first_range = 0;
    std::uint64_t ect0 = 0;
    std::uint64_t ect1 = 0;
    std::uint64_t ce = 0;
    std::uint64_t ranges_length = 0;
};

struct QuicCryptoFrame {
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
};

struct QuicNewTokenFrame {
    std::uint64_t length = 0;
};

struct QuicStreamFrame {
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    std::uint64_t stream_id = 0;
    bool has_offset = false;
    bool has_length = false;
    bool fin = false;
};

struct QuicCloseFrame {
    std::uint64_t error_code = 0;
    std::uint64_t frame_type = 0;
    QuicSlice reason{};
};

struct QuicResetStreamFrame {
    std::uint64_t id = 0;
    std::uint64_t error_code = 0;
    std::uint64_t final_size = 0;
};

struct QuicStopSendingFrame {
    std::uint64_t id = 0;
    std::uint64_t error_code = 0;
};

struct QuicMaxDataFrame {
    std::uint64_t max_data = 0;
};

struct QuicMaxStreamDataFrame {
    std::uint64_t id = 0;
    std::uint64_t limit = 0;
};

struct QuicMaxStreamsFrame {
    std::uint64_t limit = 0;
    bool bidirectional = true;
};

struct QuicDataBlockedFrame {
    std::uint64_t limit = 0;
};

struct QuicStreamDataBlockedFrame {
    std::uint64_t id = 0;
    std::uint64_t limit = 0;
};

struct QuicStreamsBlockedFrame {
    std::uint64_t limit = 0;
    bool bidirectional = true;
};

struct QuicNewConnectionIdFrame {
    std::uint64_t sequence_number = 0;
    std::uint64_t retire_prior_to = 0;
    std::uint8_t cid_len = 0;
    std::uint8_t cid[kMaxConnectionIdLength]{};
    std::uint8_t stateless_reset_token[kStatelessResetTokenLength]{};
};

struct QuicRetireConnectionIdFrame {
    std::uint64_t sequence_number = 0;
};

struct QuicPathChallengeFrame {
    std::uint8_t data[8]{};
};

struct QuicFrame {
    QuicFrame() noexcept : u{} {}

    QuicFrameType type = QuicFrameType::Padding;
    QuicEncryptionLevel level = QuicEncryptionLevel::Initial;
    std::uint64_t packet_number = 0;
    bool ack_eliciting = false;
    QuicSlice data{};

    union Payload {
        QuicPaddingFrame padding;
        QuicAckFrame ack;
        QuicCryptoFrame crypto;
        QuicNewTokenFrame new_token;
        QuicStreamFrame stream;
        QuicCloseFrame close;
        QuicResetStreamFrame reset_stream;
        QuicStopSendingFrame stop_sending;
        QuicMaxDataFrame max_data;
        QuicMaxStreamDataFrame max_stream_data;
        QuicMaxStreamsFrame max_streams;
        QuicDataBlockedFrame data_blocked;
        QuicStreamDataBlockedFrame stream_data_blocked;
        QuicStreamsBlockedFrame streams_blocked;
        QuicNewConnectionIdFrame new_connection_id;
        QuicRetireConnectionIdFrame retire_connection_id;
        QuicPathChallengeFrame path_challenge;
        QuicPathChallengeFrame path_response;
    } u;

    common::IntrusiveListHook queue_hook{};
};

struct QuicFrameParseResult {
    QuicFrame frame{};
    std::size_t consumed = 0;
};

[[nodiscard]] inline bool quic_is_long_packet(std::uint8_t flags) noexcept { return (flags & kPacketFlagLong) != 0; }

[[nodiscard]] inline bool quic_is_short_packet(std::uint8_t flags) noexcept { return !quic_is_long_packet(flags); }

[[nodiscard]] inline bool quic_is_stream_frame_type(std::uint64_t type) noexcept {
    return type >= static_cast<std::uint64_t>(QuicFrameType::Stream) &&
           type <= static_cast<std::uint64_t>(QuicFrameType::Stream7);
}

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_PROTOCOL_H
