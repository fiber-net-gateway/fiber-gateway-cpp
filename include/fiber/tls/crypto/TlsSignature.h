#ifndef FIBER_TLS_CRYPTO_TLS_SIGNATURE_H
#define FIBER_TLS_CRYPTO_TLS_SIGNATURE_H

// TLS signature primitives (feature/tls/02b §3): private-key loading, the
// scheme<->key matching table, and sign/verify over pre-built signature
// contents. Pure primitives — the CONTENT construction (the TLS 1.3 context
// string, the TLS 1.2 CR||SR||params concatenation, the 1.2 client
// digest-of-transcript) belongs to the 06/07 handshake engines and is pinned
// as a contract in the design doc, not here. Digest choice, RSA-PSS
// parameters and the Ed25519 NULL digest are dispatched from the scheme.
//
// The signature path mirrors BoringSSL's own (ssl_privkey.cc setup_ctx /
// ssl_pkey_supports_algorithm): EVP_DigestSignInit/VerifyInit, PSS with
// RSA_PKCS1_PSS_PADDING + RSA_PSS_SALTLEN_DIGEST, one-shot EVP_DigestSign.
// ECDSA signature values are DER both on the EVP and the TLS wire side — no
// raw r||s conversion anywhere. No OpenSSL type appears in this header (the
// handles are void* pimpls, freed on the .cpp side).

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../common/IoError.h"
#include "../TlsVersion.h"
#include "../handshake/TlsCipherSuites.h"

namespace fiber::tls {

// Key families the matching table distinguishes. EC curve checks are a
// per-scheme constraint inside supports() (TLS 1.3 only, mirroring
// ssl_pkey_supports_algorithm), not a kind of their own.
enum class TlsKeyKind : std::uint8_t { Rsa, Ec, Ed25519 };

// Borrowed, non-owning public key. The lifetime is backed by the
// TlsCertificate it came from — the underlying pointer is X509_get0_pubkey's
// internal reference, so the view is exactly as alive as the certificate.
class TlsPublicKeyView {
public:
    TlsPublicKeyView() noexcept = default;

    [[nodiscard]] TlsKeyKind key_kind() const noexcept;
    // Same matching rules as TlsPrivateKey::supports — used to pick/validate
    // the scheme for a CertificateVerify received from the peer.
    [[nodiscard]] bool supports(TlsSignatureScheme scheme, TlsProtocolVersion version) const noexcept;
    [[nodiscard]] bool empty() const noexcept { return impl_ == nullptr; }

private:
    friend class TlsCertificate;
    friend common::IoResult<bool> tls_verify(TlsSignatureScheme, TlsProtocolVersion, const TlsPublicKeyView &,
                                             std::span<const std::uint8_t>, std::span<const std::uint8_t>) noexcept;
    explicit TlsPublicKeyView(const void *impl) noexcept : impl_(impl) {}

    const void *impl_ = nullptr; // const EVP_PKEY*
};

// Owning private key (EVP_PKEY* pimpl, freed in the .cpp). Move-only. Loaded
// once at configuration time and shared immutably across workers afterwards.
class TlsPrivateKey {
public:
    TlsPrivateKey() noexcept = default;
    TlsPrivateKey(const TlsPrivateKey &) = delete;
    TlsPrivateKey &operator=(const TlsPrivateKey &) = delete;
    TlsPrivateKey(TlsPrivateKey &&other) noexcept;
    TlsPrivateKey &operator=(TlsPrivateKey &&other) noexcept;
    ~TlsPrivateKey();

    // DER: PKCS#8 oneAsymmetricKey preferred; the traditional PKCS#1 (RSA)
    // and sec1 (EC) encodings are auto-detected by the parser (BoringSSL
    // d2i_AutoPrivateKey probes PKCS#8, then by element count). Only
    // unencrypted material — encrypted PKCS#8 fails to parse (Invalid).
    [[nodiscard]] static common::IoResult<TlsPrivateKey> parse_der(std::span<const std::uint8_t> der) noexcept;
    // PEM: "PRIVATE KEY" / "RSA PRIVATE KEY" / "EC PRIVATE KEY" blocks,
    // armor stripped without BIO. Encrypted PEM headers are rejected.
    [[nodiscard]] static common::IoResult<TlsPrivateKey> parse_pem(std::span<const char> pem) noexcept;

    [[nodiscard]] TlsKeyKind key_kind() const noexcept;
    // Scheme<->key matching (02b §3.2, mirrors ssl_pkey_supports_algorithm):
    // pkey type, RSA-PSS modulus length (>= 2*hash_len + 2), the TLS 1.3 EC
    // curve NID check, and the tls12_ok/tls13_ok version gates
    // (rsa_pkcs1_* never signs a 1.3 handshake).
    [[nodiscard]] bool supports(TlsSignatureScheme scheme, TlsProtocolVersion version) const noexcept;
    // Upper bound for a signature over this key (EVP_PKEY_size: the RSA
    // modulus length; EC/Ed25519 DER values are smaller). Size the engine's
    // stack buffer with this.
    [[nodiscard]] std::size_t max_signature_len() const noexcept;
    [[nodiscard]] bool empty() const noexcept { return impl_ == nullptr; }

    // Signs |content| (the engine-built string per 02b §3.4) writing the
    // wire-format signature to |out| (capacity >= max_signature_len()) and
    // returning its length. FIBER_ASSERTs supports(scheme, version): the
    // engine must have selected the scheme through supports() — a violation
    // is an engine bug, not a protocol event.
    [[nodiscard]] common::IoResult<std::size_t> sign(TlsSignatureScheme scheme, TlsProtocolVersion version,
                                                     std::span<const std::uint8_t> content,
                                                     std::span<std::uint8_t> out) const noexcept;

private:
    friend class TlsCertificate; // matches_private_key compares the raw handles
    void *impl_ = nullptr; // EVP_PKEY*
};

// Verifies a peer signature. IoResult<bool>: true = valid, false = invalid
// signature (a protocol event — the engine raises decrypt_error), an IoErr
// return means the scheme does not match the key / is outside the implemented
// set (the engine raises illegal_parameter). The verify side never asserts:
// the peer's scheme choice is untrusted input.
[[nodiscard]] common::IoResult<bool> tls_verify(TlsSignatureScheme scheme, TlsProtocolVersion version,
                                                const TlsPublicKeyView &public_key,
                                                std::span<const std::uint8_t> content,
                                                std::span<const std::uint8_t> signature) noexcept;

// Negotiation preferences (02b §3.1): the 06 selection loop walks these,
// taking the first scheme the peer offered and the local key supports.
// Aligned with BoringSSL's kSignSignatureAlgorithms (extensions.cc) with the
// SHA-1 entries dropped — Ed25519 and P-256 lead, then the SHA-384 group,
// then SHA-512; rsa_pkcs1_* is TLS 1.2-only fallback.
inline constexpr std::array<TlsSignatureScheme, 7> kTls13SignaturePreference{
        TlsSignatureScheme::Ed25519,          TlsSignatureScheme::EcdsaSecp256r1Sha256,
        TlsSignatureScheme::RsaPssRsaeSha256, TlsSignatureScheme::EcdsaSecp384r1Sha384,
        TlsSignatureScheme::RsaPssRsaeSha384, TlsSignatureScheme::EcdsaSecp521r1Sha512,
        TlsSignatureScheme::RsaPssRsaeSha512,
};
inline constexpr std::array<TlsSignatureScheme, 10> kTls12SignaturePreference{
        TlsSignatureScheme::Ed25519,
        TlsSignatureScheme::EcdsaSecp256r1Sha256,
        TlsSignatureScheme::RsaPssRsaeSha256,
        TlsSignatureScheme::RsaPkcs1Sha256,
        TlsSignatureScheme::EcdsaSecp384r1Sha384,
        TlsSignatureScheme::RsaPssRsaeSha384,
        TlsSignatureScheme::RsaPkcs1Sha384,
        TlsSignatureScheme::EcdsaSecp521r1Sha512,
        TlsSignatureScheme::RsaPssRsaeSha512,
        TlsSignatureScheme::RsaPkcs1Sha512,
};

} // namespace fiber::tls

#endif // FIBER_TLS_CRYPTO_TLS_SIGNATURE_H
