#ifndef FIBER_TLS_HANDSHAKE_TLS_SERVER_HANDSHAKE_SHARED_H
#define FIBER_TLS_HANDSHAKE_TLS_SERVER_HANDSHAKE_SHARED_H

// Version-neutral pieces shared by the server handshake engine's outer shell
// and its two version sub-flows (07 §6.1) — the mirror of the client's
// TlsClientHandshakeShared. Internal to src/tls.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include <fiber/tls/TlsConfig.h>
#include <fiber/tls/crypto/Tls13KeySchedule.h>
#include <fiber/tls/crypto/TlsKeyExchange.h>
#include <fiber/tls/crypto/TlsSignature.h>
#include <fiber/tls/handshake/TlsHandshakeMessage.h>
#include <fiber/tls/record/TlsRecordCipher.h>

namespace fiber::tls {

// ---- engine-fixed preference tables (§10.2: server preference order,
// registry constants — not config) ----

// Server suite preference: the 1.3 set first, then the ECDHE+AEAD 1.2 set.
// Semantics differ from the client's identically-valued offer table: the
// server walks THIS order and takes the first suite the client offered.
inline constexpr std::array<std::uint16_t, 9> kServerSuites{
        0x1301, 0x1302, 0x1303, 0xC02F, 0xC030, 0xCCA8, 0xC02B, 0xC02C, 0xCCA9,
};

// Server group preference: X25519 leads; P-256 is the fallback (and the HRR
// answer when the client's first share misses).
inline constexpr std::array<std::uint16_t, 2> kServerGroups{0x001D, 0x0017};

// The CertificateRequest signature_algorithms offer = the 02b 1.3 preference
// (what we can verify; the client's CV scheme must come from this list).
inline constexpr auto kServerCrSigalgs = [] {
    std::array<std::uint16_t, kTls13SignaturePreference.size()> out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::uint16_t>(kTls13SignaturePreference[i]);
    }
    return out;
}();

// Same, the 1.2 flavor: the 02b 1.2 preference (RSA-PKCS1/PSS + ECDSA over
// SHA-2; what we can verify at 1.2 semantics).
inline constexpr auto kServerCr12Sigalgs = [] {
    std::array<std::uint16_t, kTls12SignaturePreference.size()> out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::uint16_t>(kTls12SignaturePreference[i]);
    }
    return out;
}();

// Shared bounds (both sub-flows stage their flights in the outer-owned
// scratch; an RSA-4096 signature bound mirrors the client side).
inline constexpr std::size_t kServerMaxSigLen = 1024;

// ---- 0-RTT / resumption bounds (§10.5/§10.4; BoringSSL-mirrored) ----

// Accepted-early-data ceiling: plaintext CONTENT bytes per connection
// (RFC 8446 §4.6.1 counts content only). Matches BoringSSL's
// kMaxEarlyDataAccepted (internal.h) — also the max_early_data value minted
// into NSTs when enable_early_data is on.
inline constexpr std::uint32_t kMaxEarlyDataAccepted = 14336;

// Rejected-early-data discard ceiling: CIPHERTEXT record bytes (the
// plaintext of records we cannot open is unknowable). BoringSSL's
// kMaxEarlyDataSkipped.
inline constexpr std::size_t kMaxEarlyDataSkipped = 16384;

// Ticket-age tolerance: |client_age − server_age| beyond this rejects
// resumption (the handshake continues as a full one, §10.4).
inline constexpr std::uint64_t kMaxTicketAgeSkewMs = 60'000;

// A single pre_shared_key identity larger than this is a malformed offer
// (BoringSSL's bound); the ticket is rejected, not the connection.
inline constexpr std::size_t kMaxPskIdentityLen = 4096;

// ---- pre-fork state owned by the outer shell, borrowed by the sub-flows ----

// The retained ClientHello (raw body bytes) and its negotiation
// intermediates. The shell copies the CH body here BEFORE decoding so the
// decoded view's spans survive past the step() borrow; the forked sub-flow
// borrows it mutably (the HRR path re-decodes CH2 over the same storage).
struct TlsServerHelloState {
    static constexpr std::size_t kCap = 16384; // retained ClientHello body bytes

    std::array<std::uint8_t, kCap> ch{};
    std::size_t ch_len = 0;
    TlsClientHello view; // decoded; spans borrow ch
    std::unique_ptr<TlsKeyExchange> kx; // the server's share (encap'd at SH build)
    TlsNamedGroup kx_group = TlsNamedGroup::X25519; // group of kx
    std::array<std::uint8_t, 32> server_random{};
};

// Terminal handshake outcome — written by whichever sub-flow ran (or by the
// outer shell before the fork), read by the public API. Same shape as the
// client's.
struct TlsServerHandshakeOutcome {
    bool done = false;
    bool failed = false;
    TlsAlertDesc alert = TlsAlertDesc::InternalError;
};

// ---- negotiation helpers (Shared.cpp; all walk server preference × client
// offer, returning false = no common value) ----

// Suite: first kServerSuites entry (filtered to the negotiated version) that
// appears in the CH's raw cipher_suites list.
[[nodiscard]] bool tls_server_suite_select(const TlsClientHello &ch, bool tls13, TlsCipherSuiteId &out) noexcept;

// Suite (1.2): additionally filters by the credential's key kind — an RSA
// key signs the ECDHE-RSA suites, a P-256/384 key the ECDHE-ECDSA ones; an
// Ed25519 credential cannot sign in 1.2 at all (no suite survives).
[[nodiscard]] bool tls_server_suite_select_12(const TlsClientHello &ch, const TlsPrivateKey &key,
                                              TlsCipherSuiteId &out) noexcept;

// Group: first kServerGroups entry in the CH's raw supported_groups list.
[[nodiscard]] bool tls_server_group_select(const TlsClientHello &ch, TlsNamedGroup &out) noexcept;

// ALPN outcome: None (nothing negotiated — no extension in EE), Matched, or
// Failed (server config non-empty, client offered, empty intersection →
// no_application_protocol per RFC 7301 §3.2).
enum class TlsServerAlpnResult : std::uint8_t { None, Matched, Failed };
[[nodiscard]] TlsServerAlpnResult tls_server_alpn_select(const TlsServerConfig &cfg, const TlsClientHello &ch,
                                                         std::string_view &out) noexcept;

// CertificateVerify scheme: first kTls13SignaturePreference/kTls12Signature-
// Preference entry the local key supports that the CH offered.
[[nodiscard]] bool tls_server_cv_scheme_select(const TlsPrivateKey &key, const TlsClientHello &ch,
                                               TlsProtocolVersion version, TlsSignatureScheme &out) noexcept;

// Raw u16 membership over a CH list span (cipher_suites / supported_groups /
// signature_algorithms share the encoding).
[[nodiscard]] bool tls_server_list_contains(std::span<const std::uint8_t> raw_list, std::uint16_t value) noexcept;

} // namespace fiber::tls

#endif // FIBER_TLS_HANDSHAKE_TLS_SERVER_HANDSHAKE_SHARED_H
