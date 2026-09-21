#ifndef FIBER_TLS_HANDSHAKE_TLS_CIPHER_SUITES_H
#define FIBER_TLS_HANDSHAKE_TLS_CIPHER_SUITES_H

#include <array>
#include <cstdint>

namespace fiber::tls {

// Cipher suite ids this stack implements (TLS 1.3 AEAD-only suites plus the
// TLS 1.2 ECDHE+AEAD suites per the scope in feature/tls/01). Raw ids from the
// wire are compared against these; unknown ids are skipped during negotiation.
enum class TlsCipherSuiteId : std::uint16_t {
    TlsAes128GcmSha256 = 0x1301,
    TlsAes256GcmSha384 = 0x1302,
    TlsChacha20Poly1305Sha256 = 0x1303,
    EcdheEcdsaAes128GcmSha256 = 0xC02B,
    EcdheEcdsaAes256GcmSha384 = 0xC02C,
    EcdheRsaAes128GcmSha256 = 0xC02F,
    EcdheRsaAes256GcmSha384 = 0x0030,
    EcdheEcdsaChacha20Poly1305 = 0xCCA9,
    EcdheRsaChacha20Poly1305 = 0xCCA8,
};

// Named groups for (EC)DHE key exchange.
enum class TlsNamedGroup : std::uint16_t {
    Secp256r1 = 0x0017,
    Secp384r1 = 0x0018,
    X25519 = 0x001D,
    Ffdhe2048 = 0x0100,
};

// Signature schemes accepted for CertificateVerify.
enum class TlsSignatureScheme : std::uint16_t {
    RsaPkcs1Sha256 = 0x0401,
    RsaPkcs1Sha384 = 0x0501,
    RsaPkcs1Sha512 = 0x0601,
    EcdsaSecp256r1Sha256 = 0x0403,
    EcdsaSecp384r1Sha384 = 0x0503,
    Ed25519 = 0x0807,
    RsaPssRsaeSha256 = 0x0804,
    RsaPssRsaeSha384 = 0x0805,
    RsaPssRsaeSha512 = 0x0806,
};

// Hash algorithms behind the suites. A module-local enum so no OpenSSL type
// leaks past the crypto adapter (src/tls/crypto/TlsCryptoPrimitives.h maps it
// to the digest constructors).
enum class TlsHashAlgorithm : std::uint8_t { Sha256, Sha384 };

// AEAD primitives behind the suites; the record cipher maps these to the
// BoringSSL EVP_AEAD constructors.
enum class TlsAeadAlgorithm : std::uint8_t { Aes128Gcm, Aes256Gcm, Chacha20Poly1305 };

// Per-suite parameters — the single source of truth shared by the record
// cipher (AEAD pick, key/iv lengths), the key schedule (hash, key length) and
// the negotiation logic (04/06/07). IV lengths are per-kind constants, not
// per-suite data: 1.3 uses a 12-byte static IV, 1.2 a 4-byte fixed IV, and the
// record cipher owns both. The 1.2 PRF hash equals |hash| (RFC 5246 §6.1:
// suites whose name ends in SHA384 use the SHA-384 PRF; the same hash sizes
// the handshake hash that feeds verify_data).
struct TlsSuiteInfo {
    TlsCipherSuiteId suite;
    TlsAeadAlgorithm aead;
    TlsHashAlgorithm hash;
    std::uint8_t key_len; // 16 | 32
    bool is_tls13; // pairs with TlsRecordProtectionKind at cipher init
};

inline constexpr std::array<TlsSuiteInfo, 9> kTlsSuiteRegistry{
        TlsSuiteInfo{TlsCipherSuiteId::TlsAes128GcmSha256, TlsAeadAlgorithm::Aes128Gcm, TlsHashAlgorithm::Sha256, 16,
                     true},
        TlsSuiteInfo{TlsCipherSuiteId::TlsAes256GcmSha384, TlsAeadAlgorithm::Aes256Gcm, TlsHashAlgorithm::Sha384, 32,
                     true},
        TlsSuiteInfo{TlsCipherSuiteId::TlsChacha20Poly1305Sha256, TlsAeadAlgorithm::Chacha20Poly1305,
                     TlsHashAlgorithm::Sha256, 32, true},
        TlsSuiteInfo{TlsCipherSuiteId::EcdheEcdsaAes128GcmSha256, TlsAeadAlgorithm::Aes128Gcm, TlsHashAlgorithm::Sha256,
                     16, false},
        TlsSuiteInfo{TlsCipherSuiteId::EcdheEcdsaAes256GcmSha384, TlsAeadAlgorithm::Aes256Gcm, TlsHashAlgorithm::Sha384,
                     32, false},
        TlsSuiteInfo{TlsCipherSuiteId::EcdheRsaAes128GcmSha256, TlsAeadAlgorithm::Aes128Gcm, TlsHashAlgorithm::Sha256,
                     16, false},
        TlsSuiteInfo{TlsCipherSuiteId::EcdheRsaAes256GcmSha384, TlsAeadAlgorithm::Aes256Gcm, TlsHashAlgorithm::Sha384,
                     32, false},
        TlsSuiteInfo{TlsCipherSuiteId::EcdheEcdsaChacha20Poly1305, TlsAeadAlgorithm::Chacha20Poly1305,
                     TlsHashAlgorithm::Sha256, 32, false},
        TlsSuiteInfo{TlsCipherSuiteId::EcdheRsaChacha20Poly1305, TlsAeadAlgorithm::Chacha20Poly1305,
                     TlsHashAlgorithm::Sha256, 32, false},
};

// Registry lookup; nullptr for suites outside the implemented set.
[[nodiscard]] constexpr const TlsSuiteInfo *tls_suite_info(TlsCipherSuiteId suite) noexcept {
    for (const TlsSuiteInfo &info: kTlsSuiteRegistry) {
        if (info.suite == suite) {
            return &info;
        }
    }
    return nullptr;
}

} // namespace fiber::tls

#endif // FIBER_TLS_HANDSHAKE_TLS_CIPHER_SUITES_H
