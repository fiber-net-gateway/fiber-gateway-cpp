#ifndef FIBER_TLS_HANDSHAKE_TLS_CIPHER_SUITES_H
#define FIBER_TLS_HANDSHAKE_TLS_CIPHER_SUITES_H

#include <array>
#include <cstdint>

namespace fiber::tls {

// Cipher suite ids this stack implements (TLS 1.3 AEAD-only suites plus the
// TLS 1.2 ECDHE+AEAD suites per the scope in feature/tls/01, plus the legacy
// 1.2 set of feature/tls/11 — client offer only). Raw ids from the wire are
// compared against these; unknown ids are skipped during negotiation.
enum class TlsCipherSuiteId : std::uint16_t {
    TlsAes128GcmSha256 = 0x1301,
    TlsAes256GcmSha384 = 0x1302,
    TlsChacha20Poly1305Sha256 = 0x1303,
    EcdheEcdsaAes128GcmSha256 = 0xC02B,
    EcdheEcdsaAes256GcmSha384 = 0xC02C,
    EcdheRsaAes128GcmSha256 = 0xC02F,
    EcdheRsaAes256GcmSha384 = 0xC030,
    EcdheEcdsaChacha20Poly1305 = 0xCCA9,
    EcdheRsaChacha20Poly1305 = 0xCCA8,
    // 1.2 legacy set (feature/tls/11): the client offers these after every
    // AEAD suite; the server never selects them.
    EcdheEcdsaAes128CbcSha = 0xC009,
    EcdheEcdsaAes256CbcSha = 0xC00A,
    EcdheRsaAes128CbcSha = 0xC013,
    EcdheRsaAes256CbcSha = 0xC014,
    EcdheRsaAes128CbcSha256 = 0xC027,
    RsaAes128GcmSha256 = 0x009C, // static RSA key exchange from here on
    RsaAes256GcmSha384 = 0x009D,
    RsaAes128CbcSha = 0x002F,
    RsaAes256CbcSha = 0x0035,
    RsaAes128CbcSha256 = 0x003C,
};

// Named groups for (EC)DHE key exchange.
enum class TlsNamedGroup : std::uint16_t {
    Secp256r1 = 0x0017,
    Secp384r1 = 0x0018,
    X25519 = 0x001D,
    Ffdhe2048 = 0x0100,
};

// Signature schemes accepted for CertificateVerify (02b §1: the ten
// implemented schemes; rsa_pss_pss_*, rsa_pkcs1_sha1 and the md5sha1 digest
// are out of scope — the enums keep gaps, they can be added later).
enum class TlsSignatureScheme : std::uint16_t {
    RsaPkcs1Sha256 = 0x0401,
    RsaPkcs1Sha384 = 0x0501,
    RsaPkcs1Sha512 = 0x0601,
    EcdsaSecp256r1Sha256 = 0x0403,
    EcdsaSecp384r1Sha384 = 0x0503,
    EcdsaSecp521r1Sha512 = 0x0603,
    Ed25519 = 0x0807,
    RsaPssRsaeSha256 = 0x0804,
    RsaPssRsaeSha384 = 0x0805,
    RsaPssRsaeSha512 = 0x0806,
};

// Hash algorithms behind the suites. A module-local enum so no OpenSSL type
// leaks past the crypto adapter (src/tls/crypto/TlsCryptoPrimitives.h maps it
// to the digest constructors).
enum class TlsHashAlgorithm : std::uint8_t { Sha256, Sha384 };

// Record-protection primitives behind the suites; the record cipher maps
// these to the BoringSSL EVP_AEAD constructors. The Cbc entries are
// BoringSSL's TLS-specific CBC AEADs (MAC-then-encrypt, HMAC-SHA1/SHA256):
// the 1.2 legacy suites of feature/tls/11.
enum class TlsAeadAlgorithm : std::uint8_t {
    Aes128Gcm,
    Aes256Gcm,
    Chacha20Poly1305,
    Aes128CbcSha1,
    Aes256CbcSha1,
    Aes128CbcSha256,
};

// The record MAC length of a CBC algorithm — its HMAC output, which is also
// the MAC key length in the key_block; 0 for the AEADs (no separate MAC).
[[nodiscard]] constexpr std::uint8_t tls_record_mac_len(TlsAeadAlgorithm aead) noexcept {
    switch (aead) {
        case TlsAeadAlgorithm::Aes128CbcSha1:
        case TlsAeadAlgorithm::Aes256CbcSha1:
            return 20;
        case TlsAeadAlgorithm::Aes128CbcSha256:
            return 32;
        default:
            return 0;
    }
}

// The key exchange / authentication halves of a 1.2 suite name (ECDHE-RSA vs
// ECDHE-ECDSA vs static RSA). None for the 1.3 suites, which carry neither.
// Rsa: no ServerKeyExchange; the client encrypts the premaster secret to the
// server certificate's RSA key (RFC 5246 §7.4.7.1) — client side only.
enum class TlsSuiteKx : std::uint8_t { None, Ecdhe, Rsa };
enum class TlsSuiteAuth : std::uint8_t { None, Rsa, Ecdsa };

// Per-suite parameters — the single source of truth shared by the record
// cipher (AEAD pick, key/iv lengths), the key schedule (hash, key length) and
// the negotiation logic (04/06/07). IV lengths are per-kind constants, not
// per-suite data: 1.3 uses a 12-byte static IV, 1.2 a 4-byte fixed IV (GCM),
// a 12-byte implicit one (ChaCha20) or none (CBC: the IV is explicit per
// record), and the record cipher owns them. The 1.2 PRF hash equals |hash|
// (RFC 5246 §6.1: suites whose name ends in SHA384 use the SHA-384 PRF, every
// other 1.2 suite SHA-256; the same hash sizes the handshake hash that feeds
// verify_data). A CBC suite's record MAC is a separate hash, named by |aead|.
struct TlsSuiteInfo {
    TlsCipherSuiteId suite;
    TlsAeadAlgorithm aead;
    TlsHashAlgorithm hash;
    std::uint8_t key_len; // encryption key: 16 | 32 (a CBC MAC key comes on top, tls_record_mac_len)
    bool is_tls13; // pairs with TlsRecordProtectionKind at cipher init
    TlsSuiteKx kx;
    TlsSuiteAuth auth; // the credential kind the server authenticates with
};

inline constexpr std::array<TlsSuiteInfo, 19> kTlsSuiteRegistry{
        TlsSuiteInfo{TlsCipherSuiteId::TlsAes128GcmSha256, TlsAeadAlgorithm::Aes128Gcm, TlsHashAlgorithm::Sha256, 16,
                     true, TlsSuiteKx::None, TlsSuiteAuth::None},
        TlsSuiteInfo{TlsCipherSuiteId::TlsAes256GcmSha384, TlsAeadAlgorithm::Aes256Gcm, TlsHashAlgorithm::Sha384, 32,
                     true, TlsSuiteKx::None, TlsSuiteAuth::None},
        TlsSuiteInfo{TlsCipherSuiteId::TlsChacha20Poly1305Sha256, TlsAeadAlgorithm::Chacha20Poly1305,
                     TlsHashAlgorithm::Sha256, 32, true, TlsSuiteKx::None, TlsSuiteAuth::None},
        TlsSuiteInfo{TlsCipherSuiteId::EcdheEcdsaAes128GcmSha256, TlsAeadAlgorithm::Aes128Gcm, TlsHashAlgorithm::Sha256,
                     16, false, TlsSuiteKx::Ecdhe, TlsSuiteAuth::Ecdsa},
        TlsSuiteInfo{TlsCipherSuiteId::EcdheEcdsaAes256GcmSha384, TlsAeadAlgorithm::Aes256Gcm, TlsHashAlgorithm::Sha384,
                     32, false, TlsSuiteKx::Ecdhe, TlsSuiteAuth::Ecdsa},
        TlsSuiteInfo{TlsCipherSuiteId::EcdheRsaAes128GcmSha256, TlsAeadAlgorithm::Aes128Gcm, TlsHashAlgorithm::Sha256,
                     16, false, TlsSuiteKx::Ecdhe, TlsSuiteAuth::Rsa},
        TlsSuiteInfo{TlsCipherSuiteId::EcdheRsaAes256GcmSha384, TlsAeadAlgorithm::Aes256Gcm, TlsHashAlgorithm::Sha384,
                     32, false, TlsSuiteKx::Ecdhe, TlsSuiteAuth::Rsa},
        TlsSuiteInfo{TlsCipherSuiteId::EcdheEcdsaChacha20Poly1305, TlsAeadAlgorithm::Chacha20Poly1305,
                     TlsHashAlgorithm::Sha256, 32, false, TlsSuiteKx::Ecdhe, TlsSuiteAuth::Ecdsa},
        TlsSuiteInfo{TlsCipherSuiteId::EcdheRsaChacha20Poly1305, TlsAeadAlgorithm::Chacha20Poly1305,
                     TlsHashAlgorithm::Sha256, 32, false, TlsSuiteKx::Ecdhe, TlsSuiteAuth::Rsa},
        TlsSuiteInfo{TlsCipherSuiteId::EcdheEcdsaAes128CbcSha, TlsAeadAlgorithm::Aes128CbcSha1,
                     TlsHashAlgorithm::Sha256, 16, false, TlsSuiteKx::Ecdhe, TlsSuiteAuth::Ecdsa},
        TlsSuiteInfo{TlsCipherSuiteId::EcdheEcdsaAes256CbcSha, TlsAeadAlgorithm::Aes256CbcSha1,
                     TlsHashAlgorithm::Sha256, 32, false, TlsSuiteKx::Ecdhe, TlsSuiteAuth::Ecdsa},
        TlsSuiteInfo{TlsCipherSuiteId::EcdheRsaAes128CbcSha, TlsAeadAlgorithm::Aes128CbcSha1, TlsHashAlgorithm::Sha256,
                     16, false, TlsSuiteKx::Ecdhe, TlsSuiteAuth::Rsa},
        TlsSuiteInfo{TlsCipherSuiteId::EcdheRsaAes256CbcSha, TlsAeadAlgorithm::Aes256CbcSha1, TlsHashAlgorithm::Sha256,
                     32, false, TlsSuiteKx::Ecdhe, TlsSuiteAuth::Rsa},
        TlsSuiteInfo{TlsCipherSuiteId::EcdheRsaAes128CbcSha256, TlsAeadAlgorithm::Aes128CbcSha256,
                     TlsHashAlgorithm::Sha256, 16, false, TlsSuiteKx::Ecdhe, TlsSuiteAuth::Rsa},
        TlsSuiteInfo{TlsCipherSuiteId::RsaAes128GcmSha256, TlsAeadAlgorithm::Aes128Gcm, TlsHashAlgorithm::Sha256, 16,
                     false, TlsSuiteKx::Rsa, TlsSuiteAuth::Rsa},
        TlsSuiteInfo{TlsCipherSuiteId::RsaAes256GcmSha384, TlsAeadAlgorithm::Aes256Gcm, TlsHashAlgorithm::Sha384, 32,
                     false, TlsSuiteKx::Rsa, TlsSuiteAuth::Rsa},
        TlsSuiteInfo{TlsCipherSuiteId::RsaAes128CbcSha, TlsAeadAlgorithm::Aes128CbcSha1, TlsHashAlgorithm::Sha256, 16,
                     false, TlsSuiteKx::Rsa, TlsSuiteAuth::Rsa},
        TlsSuiteInfo{TlsCipherSuiteId::RsaAes256CbcSha, TlsAeadAlgorithm::Aes256CbcSha1, TlsHashAlgorithm::Sha256, 32,
                     false, TlsSuiteKx::Rsa, TlsSuiteAuth::Rsa},
        TlsSuiteInfo{TlsCipherSuiteId::RsaAes128CbcSha256, TlsAeadAlgorithm::Aes128CbcSha256, TlsHashAlgorithm::Sha256,
                     16, false, TlsSuiteKx::Rsa, TlsSuiteAuth::Rsa},
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
