#ifndef FIBER_TLS_TLS_VERSION_H
#define FIBER_TLS_TLS_VERSION_H

#include <cstdint>
#include <span>

namespace fiber::tls {

// Wire ProtocolVersion values (legacy_record_version shares the space but is
// handled separately by the record layer).
enum class TlsProtocolVersion : std::uint16_t {
    Ssl30 = 0x0300,
    Tls10 = 0x0301,
    Tls11 = 0x0302,
    Tls12 = 0x0303,
    Tls13 = 0x0304,
};

inline constexpr std::uint16_t kTlsVersionTls12 = 0x0303;
inline constexpr std::uint16_t kTlsVersionTls13 = 0x0304;

[[nodiscard]] inline bool tls_is_known_version(std::uint16_t version) noexcept {
    switch (static_cast<TlsProtocolVersion>(version)) {
        case TlsProtocolVersion::Tls10:
        case TlsProtocolVersion::Tls11:
        case TlsProtocolVersion::Tls12:
        case TlsProtocolVersion::Tls13:
            return true;
        case TlsProtocolVersion::Ssl30:
            return false;
        default:
            return false;
    }
}

// Membership test over a supported_versions list payload (raw u16 entries,
// even length, structurally validated by the ClientHello decoder).
[[nodiscard]] bool tls_version_list_contains(std::span<const std::uint8_t> versions, std::uint16_t version) noexcept;

} // namespace fiber::tls

#endif // FIBER_TLS_TLS_VERSION_H
