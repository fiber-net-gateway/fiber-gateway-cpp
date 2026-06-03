#ifndef FIBER_QUIC_QUIC_PROTOCOL_H
#define FIBER_QUIC_QUIC_PROTOCOL_H

#include <cstddef>
#include <cstdint>

#include "QuicConnection.h"
#include "QuicFrame.h"

namespace fiber::quic {

inline constexpr std::uint32_t kQuicVersion1 = 0x00000001;
inline constexpr std::size_t kMinInitialDatagramSize = 1200;
inline constexpr std::size_t kAeadTagLength = 16;
inline constexpr std::size_t kHeaderProtectionMaskLength = 5;
inline constexpr std::size_t kHeaderProtectionSampleLength = 16;
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

[[nodiscard]] inline bool quic_is_long_packet(std::uint8_t flags) noexcept { return (flags & kPacketFlagLong) != 0; }

[[nodiscard]] inline bool quic_is_short_packet(std::uint8_t flags) noexcept { return !quic_is_long_packet(flags); }

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_PROTOCOL_H
