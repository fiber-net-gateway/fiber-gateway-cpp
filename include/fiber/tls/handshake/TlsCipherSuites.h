#ifndef FIBER_TLS_HANDSHAKE_TLS_CIPHER_SUITES_H
#define FIBER_TLS_HANDSHAKE_TLS_CIPHER_SUITES_H

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

} // namespace fiber::tls

#endif // FIBER_TLS_HANDSHAKE_TLS_CIPHER_SUITES_H
