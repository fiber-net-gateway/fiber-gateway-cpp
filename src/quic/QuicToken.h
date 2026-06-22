#ifndef FIBER_QUIC_QUIC_TOKEN_H
#define FIBER_QUIC_QUIC_TOKEN_H

#include <array>
#include <cstddef>
#include <cstdint>

#include "../common/IoError.h"
#include "../net/SocketAddress.h"
#include "QuicConnectionId.h"
#include "QuicProtocol.h"

namespace fiber::quic {

inline constexpr std::size_t kQuicAddressValidationKeyLength = 32;
inline constexpr std::size_t kQuicAddressTokenIvLength = 12;
inline constexpr std::size_t kQuicAddressTokenHashLength = 20;
inline constexpr std::size_t kQuicAddressTokenPlaintextMaxLength =
        kQuicAddressTokenHashLength + sizeof(std::uint64_t) + 1 + 1 + kMaxConnectionIdLength;
inline constexpr std::size_t kQuicAddressTokenMaxLength =
        kQuicAddressTokenIvLength + kQuicAddressTokenPlaintextMaxLength + kAeadTagLength;

enum class QuicAddressTokenKind : std::uint8_t {
    NewToken = 0,
    Retry = 1,
};

enum class QuicAddressTokenValidationStatus : std::uint8_t {
    Valid,
    Invalid,
    Expired,
    Garbage,
};

struct QuicAddressToken {
    std::array<std::uint8_t, kQuicAddressTokenMaxLength> bytes{};
    std::size_t len = 0;

    [[nodiscard]] QuicSlice slice() const noexcept { return {bytes.data(), len}; }
};

struct QuicAddressTokenValidation {
    QuicAddressTokenValidationStatus status = QuicAddressTokenValidationStatus::Garbage;
    QuicAddressTokenKind kind = QuicAddressTokenKind::NewToken;
    QuicConnectionId original_destination_connection_id{};
};

[[nodiscard]] common::IoResult<QuicAddressToken>
quic_create_address_token(const std::array<std::uint8_t, kQuicAddressValidationKeyLength> &key,
                          const net::SocketAddress &peer, std::uint64_t expires_unix_seconds, QuicAddressTokenKind kind,
                          const QuicConnectionId *original_dcid = nullptr) noexcept;

[[nodiscard]] common::IoResult<QuicAddressTokenValidation>
quic_validate_address_token(const std::array<std::uint8_t, kQuicAddressValidationKeyLength> &key,
                            const net::SocketAddress &peer, std::uint64_t now_unix_seconds, QuicSlice token) noexcept;

[[nodiscard]] std::uint64_t quic_unix_seconds_now() noexcept;

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_TOKEN_H
