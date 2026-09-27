#ifndef FIBER_TLS_HANDSHAKE_TLS_SUITE_PREFERENCE_H
#define FIBER_TLS_HANDSHAKE_TLS_SUITE_PREFERENCE_H

// The engine's suite order and its hardware-aware effective form, shared by
// the client offer (TlsClientHandshakeShared) and the server preference walk
// (TlsServerHandshakeShared). Internal to src/tls.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <fiber/tls/handshake/TlsCipherSuites.h>

namespace fiber::tls {

// The engine's suite order: the 1.3 set first (preference order), then the
// ECDHE+AEAD 1.2 set. One table serves both sides — the client offers it
// verbatim, the server walks it taking the first suite the client offered.
inline constexpr std::array<std::uint16_t, 9> kTlsSuitePreference{
        0x1301, 0x1302, 0x1303, 0xC02F, 0xC030, 0xCCA8, 0xC02B, 0xC02C, 0xCCA9,
};

// The client-only legacy tail (feature/tls/11): 1.2 suites for upstreams that
// speak none of the above, offered AFTER the whole AEAD order so a server
// supporting anything modern never picks them. The server never walks this
// table. Order mirrors BoringSSL's client list: forward-secret CBC first,
// then static-RSA GCM, then static-RSA CBC.
inline constexpr std::array<std::uint16_t, 10> kTlsClientLegacySuites{
        0xC009, 0xC013, 0xC00A, 0xC014, 0xC027, 0x009C, 0x009D, 0x002F, 0x0035, 0x003C,
};

// True when this CPU accelerates AES-GCM (BoringSSL's probe; the answer is
// library-cached).
[[nodiscard]] bool tls_has_aes_hardware() noexcept;

// Stable reorder moving every ChaCha20-Poly1305 suite ahead of the AES-GCM
// ones within each version set (the 1.3 set stays ahead of the 1.2 set).
// BoringSSL's no-AES-hardware preference: on such hosts ChaCha20-Poly1305
// seals several times faster than vpaes AES-GCM (all_benchmark_6 §7.1: bulk
// ~2.4x per byte). Registry order is preserved otherwise. The input is
// kTlsSuitePreference — an entry outside the registry would be dropped.
[[nodiscard]] constexpr std::array<std::uint16_t, 9>
tls_suites_chacha_first(std::array<std::uint16_t, 9> order) noexcept {
    std::array<std::uint16_t, 9> out{};
    std::size_t w = 0;
    for (const bool tls13_set: {true, false}) {
        for (const bool chacha: {true, false}) {
            for (const std::uint16_t raw: order) {
                const TlsSuiteInfo *info = tls_suite_info(static_cast<TlsCipherSuiteId>(raw));
                if (info == nullptr || info->is_tls13 != tls13_set ||
                    (info->aead == TlsAeadAlgorithm::Chacha20Poly1305) != chacha) {
                    continue;
                }
                out[w++] = raw;
            }
        }
    }
    return out;
}

// The effective order at runtime: kTlsSuitePreference where AES-GCM is
// hardware-accelerated, its ChaCha-first permutation otherwise. Computed
// once; both the CH offer and the server preference walk negotiate in this
// order (mirrors BoringSSL: handshake_client.cc kCiphersNoAESHardware,
// ssl_cipher.cc's has_aes_hw list ordering).
[[nodiscard]] const std::array<std::uint16_t, 9> &tls_effective_suite_order() noexcept;

// The ClientHello cipher_suites: the effective order, followed by the legacy
// tail when the offer includes TLS 1.2 (the tail is 1.2-only). Static
// storage, computed once.
[[nodiscard]] std::span<const std::uint16_t> tls_client_offer_suites(bool offer_tls12) noexcept;

} // namespace fiber::tls

#endif // FIBER_TLS_HANDSHAKE_TLS_SUITE_PREFERENCE_H
