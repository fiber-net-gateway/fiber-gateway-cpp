#ifndef FIBER_TLS_HANDSHAKE_TLS_EXTENSION_CODEC_H
#define FIBER_TLS_HANDSHAKE_TLS_EXTENSION_CODEC_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "../../common/IoError.h"
#include "../detail/TlsCursor.h"
#include "TlsCipherSuites.h"

namespace fiber::tls {

enum class TlsExtensionType : std::uint16_t {
    ServerName = 0,
    MaxFragmentLength = 1,
    StatusRequest = 5,
    SupportedGroups = 10,
    SignatureAlgorithms = 13,
    UseSrtp = 14,
    Heartbeat = 15,
    Alpn = 16,
    StatusRequestV2 = 17,
    SignedCertificateTimestamp = 18,
    ClientCertificateType = 19,
    ServerCertificateType = 20,
    Padding = 21,
    EncryptThenMac = 22,
    ExtendedMasterSecret = 23,
    RecordSizeLimit = 28,
    SessionTicket = 35,
    PreSharedKey = 41,
    EarlyData = 42,
    SupportedVersions = 43,
    Cookie = 44,
    PskKeyExchangeModes = 45,
    SignatureAlgorithmsCert = 50,
    KeyShare = 51,
    ConnectionId = 54,
    TranscriptPadding = 56,
    RenegotiationInfo = 0xFF01,
};

// One extension as seen in an extension block walk.
struct TlsExtensionView {
    std::uint16_t type = 0;
    std::span<const std::uint8_t> data{};
};

// Walks an extension block payload ({type(2), len(2), data} entries). Exhausts
// exactly at the block end; truncation or trailing bytes fail with Invalid.
class TlsExtensionCursor {
public:
    explicit TlsExtensionCursor(std::span<const std::uint8_t> block) noexcept : cursor_(block.data(), block.size()) {}

    // Engaged false: block exhausted. Disengaged: malformed entry.
    [[nodiscard]] common::IoResult<bool> next(TlsExtensionView &out) noexcept;

private:
    TlsReadCursor cursor_;
};

// --- per-extension payload helpers (each operates on validated shapes, but
// also fails cleanly when given arbitrary spans) ---

// RFC 7301 ProtocolNameList walk: entries are 1-byte-length-prefixed names.
class TlsAlpnCursor {
public:
    explicit TlsAlpnCursor(std::span<const std::uint8_t> list) noexcept : cursor_(list.data(), list.size()) {}

    // Engaged false: list exhausted. Disengaged: malformed entry.
    [[nodiscard]] common::IoResult<bool> next(std::string_view &out) noexcept;

private:
    TlsReadCursor cursor_;
};

// RFC 8446 §4.2.8 ClientHello client_shares lookup.
struct TlsKeyShareView {
    TlsNamedGroup group = TlsNamedGroup::X25519;
    std::span<const std::uint8_t> key_exchange{};
};

// Finds `group`'s entry in a client_shares list body. Engaged false: not
// present. Disengaged: malformed list.
[[nodiscard]] common::IoResult<bool> tls_find_client_key_share(std::span<const std::uint8_t> client_shares,
                                                               TlsNamedGroup group, TlsKeyShareView &out) noexcept;

// RFC 8446 §4.2.9 mode bytes (psk_ke = 0, psk_dhe_ke = 1).
inline constexpr std::uint8_t kTlsPskModePskKe = 0;
inline constexpr std::uint8_t kTlsPskModePskDheKe = 1;

[[nodiscard]] bool tls_psk_modes_contains(std::span<const std::uint8_t> modes, std::uint8_t mode) noexcept;

} // namespace fiber::tls

#endif // FIBER_TLS_HANDSHAKE_TLS_EXTENSION_CODEC_H
