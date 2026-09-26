// Hardware-aware suite preference (all_benchmark_6 §7.1): the pure
// ChaCha-first permutation, the probe-backed effective order, and the server
// preference walk over it.

#include "tls/handshake/TlsServerHandshakeShared.h" // src-side header (tests may include it)
#include "tls/handshake/TlsSuitePreference.h"

#include <algorithm>
#include <array>
#include <cstdint>

#include <gtest/gtest.h>

#include <fiber/tls/handshake/TlsHandshakeMessage.h>

namespace {

// Big-endian cipher_suites wire bytes for a raw TlsClientHello view.
template<std::size_t N>
[[nodiscard]] constexpr std::array<std::uint8_t, N * 2> suite_bytes(const std::array<std::uint16_t, N> &suites) {
    std::array<std::uint8_t, N * 2> out{};
    for (std::size_t i = 0; i < N; ++i) {
        out[i * 2] = static_cast<std::uint8_t>(suites[i] >> 8);
        out[i * 2 + 1] = static_cast<std::uint8_t>(suites[i]);
    }
    return out;
}

} // namespace

// The permutation: ChaCha20-Poly1305 leads each version set (mirrors
// BoringSSL handshake_client.cc kCiphersNoAESHardware and ssl_cipher.cc's
// has_aes_hw list ordering); everything else keeps registry order, and the
// result is a permutation of the table.
TEST(TlsSuitePreference, ChaChaFirstLeadsEachVersionSet) {
    constexpr auto kReordered = fiber::tls::tls_suites_chacha_first(fiber::tls::kTlsSuitePreference);
    static_assert(kReordered == std::array<std::uint16_t, 9>{
                                        0x1303,
                                        0x1301,
                                        0x1302,
                                        0xCCA8,
                                        0xCCA9,
                                        0xC02F,
                                        0xC030,
                                        0xC02B,
                                        0xC02C,
                                });
    EXPECT_TRUE(std::is_permutation(kReordered.begin(), kReordered.end(), fiber::tls::kTlsSuitePreference.begin()));
}

// The accessor follows the probe: registry order with AES acceleration,
// ChaCha-first without — and is computed once.
TEST(TlsSuitePreference, EffectiveOrderFollowsHardwareProbe) {
    const auto &effective = fiber::tls::tls_effective_suite_order();
    if (fiber::tls::tls_has_aes_hardware()) {
        EXPECT_EQ(fiber::tls::kTlsSuitePreference, effective);
    } else {
        EXPECT_EQ(fiber::tls::tls_suites_chacha_first(fiber::tls::kTlsSuitePreference), effective);
    }
    EXPECT_EQ(&effective, &fiber::tls::tls_effective_suite_order());
}

// The benchmark regression case (all_benchmark_6 §7.2): an AES-first client
// offer — h2load's default — still negotiates ChaCha20-Poly1305 on a host
// without AES acceleration; the server walks its own preference order.
TEST(TlsServerSuiteSelect, ServerPreferenceBeatsClientOrder) {
    constexpr auto kAesFirst = suite_bytes(std::array<std::uint16_t, 2>{0x1301, 0x1303});
    fiber::tls::TlsClientHello ch{};
    ch.cipher_suites = kAesFirst;

    fiber::tls::TlsCipherSuiteId suite{};
    ASSERT_TRUE(fiber::tls::tls_server_suite_select(ch, true, suite));
    EXPECT_EQ(fiber::tls::tls_has_aes_hardware() ? 0x1301u : 0x1303u, static_cast<std::uint16_t>(suite));
}

// The 1.2 arm of the same table: ChaCha-first ordering applies to the
// ECDHE-RSA set too.
TEST(TlsServerSuiteSelect, ServerPreferenceBeatsClientOrder12) {
    constexpr auto kAesFirst = suite_bytes(std::array<std::uint16_t, 2>{0xC02F, 0xCCA8});
    fiber::tls::TlsClientHello ch{};
    ch.cipher_suites = kAesFirst;

    fiber::tls::TlsCipherSuiteId suite{};
    ASSERT_TRUE(fiber::tls::tls_server_suite_select(ch, false, suite));
    EXPECT_EQ(fiber::tls::tls_has_aes_hardware() ? 0xC02Fu : 0xCCA8u, static_cast<std::uint16_t>(suite));
}

// No ChaCha on offer: the first AES entry wins regardless of hardware.
TEST(TlsServerSuiteSelect, AesOnlyOfferKeepsRegistryOrder) {
    constexpr auto kAesOnly = suite_bytes(std::array<std::uint16_t, 2>{0x1302, 0x1301});
    fiber::tls::TlsClientHello ch{};
    ch.cipher_suites = kAesOnly;

    fiber::tls::TlsCipherSuiteId suite{};
    ASSERT_TRUE(fiber::tls::tls_server_suite_select(ch, true, suite));
    EXPECT_EQ(0x1301u, static_cast<std::uint16_t>(suite));
}
