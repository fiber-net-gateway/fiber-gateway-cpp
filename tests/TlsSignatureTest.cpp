#include <gtest/gtest.h>

#include <openssl/evp.h>
#include <openssl/rsa.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <vector>

// TlsSignatureVectors.h references certfix:: constants but does not include
// their home — fixtures must come first.
#include "TlsCertFixtures.h"
#include "TlsSignatureVectors.h"

#include <fiber/tls/crypto/TlsCertificate.h>
#include <fiber/tls/crypto/TlsSignature.h>

using namespace fiber::tls;

namespace {

// All key/certificate material and every signature below is machine-generated
// by tests/tls_certs/gen_tls_certs.py and verified once in Python before
// emission (the 02 §12.2 pipeline discipline); the supports() matrix is the
// independently hand-pinned §3.2 oracle.

template<typename T, std::size_t N>
std::span<const T> as_span(const std::array<T, N> &a) {
    return {a.data(), a.size()};
}

std::span<const char> pem_span(const char *pem) { return {pem, std::strlen(pem)}; }

// The view borrows the certificate's cached public key, so the owning chain
// must outlive it — |out_chain| is where the parsed chain lands.
TlsPublicKeyView cert_public_key(const char *cert_pem, TlsCertificateChain &out_chain) {
    auto chain = TlsCertificateChain::parse_pem_bundle(pem_span(cert_pem));
    EXPECT_TRUE(chain.has_value());
    if (!chain.has_value()) {
        return {};
    }
    out_chain = std::move(*chain);
    auto key = out_chain.leaf().public_key();
    EXPECT_TRUE(key.has_value());
    return key.has_value() ? *key : TlsPublicKeyView{};
}

const char *scheme_name(TlsSignatureScheme scheme) {
    switch (scheme) {
        case TlsSignatureScheme::RsaPkcs1Sha256:
            return "rsa_pkcs1_sha256";
        case TlsSignatureScheme::RsaPkcs1Sha384:
            return "rsa_pkcs1_sha384";
        case TlsSignatureScheme::RsaPkcs1Sha512:
            return "rsa_pkcs1_sha512";
        case TlsSignatureScheme::EcdsaSecp256r1Sha256:
            return "ecdsa_secp256r1_sha256";
        case TlsSignatureScheme::EcdsaSecp384r1Sha384:
            return "ecdsa_secp384r1_sha384";
        case TlsSignatureScheme::EcdsaSecp521r1Sha512:
            return "ecdsa_secp521r1_sha512";
        case TlsSignatureScheme::Ed25519:
            return "ed25519";
        case TlsSignatureScheme::RsaPssRsaeSha256:
            return "rsa_pss_rsae_sha256";
        case TlsSignatureScheme::RsaPssRsaeSha384:
            return "rsa_pss_rsae_sha384";
        case TlsSignatureScheme::RsaPssRsaeSha512:
            return "rsa_pss_rsae_sha512";
    }
    return "?";
}

} // namespace

// ---------------------------------------------------------------------------
// Private-key loading
// ---------------------------------------------------------------------------

TEST(TlsPrivateKeyParse, PemFormatsAndKinds) {
    struct Case {
        const char *pem;
        TlsKeyKind kind;
    };
    const Case cases[] = {
            {certfix::kRsa2048KeyPem, TlsKeyKind::Rsa}, // PKCS#8
            {certfix::kRsa1024KeyPem, TlsKeyKind::Rsa}, // PKCS#8
            {certfix::kP256KeyPem, TlsKeyKind::Ec}, // PKCS#8
            {certfix::kP384KeyPem, TlsKeyKind::Ec}, // PKCS#8
            {certfix::kP521KeyPem, TlsKeyKind::Ec}, // PKCS#8
            {certfix::kEd25519KeyPem, TlsKeyKind::Ed25519}, // PKCS#8
            {certfix::kRsa2048KeyPkcs1Pem, TlsKeyKind::Rsa}, // traditional PKCS#1
            {certfix::kP256KeySec1Pem, TlsKeyKind::Ec}, // traditional sec1
    };
    for (const Case &c: cases) {
        auto key = TlsPrivateKey::parse_pem(pem_span(c.pem));
        ASSERT_TRUE(key.has_value()) << "parse failed";
        EXPECT_FALSE(key->empty());
        EXPECT_EQ(c.kind, key->key_kind());
        EXPECT_GT(key->max_signature_len(), 0u);
    }
}

TEST(TlsPrivateKeyParse, RejectsBadInput) {
    EXPECT_EQ(TlsPrivateKey::parse_pem(pem_span("")).error(), fiber::common::IoErr::Invalid);
    EXPECT_EQ(TlsPrivateKey::parse_pem(pem_span("not a pem at all")).error(), fiber::common::IoErr::Invalid);
    // Encrypted PKCS#8 ("ENCRYPTED PRIVATE KEY" label) is not one of our labels.
    EXPECT_EQ(TlsPrivateKey::parse_pem(pem_span(certfix::kRsa2048KeyEncryptedPem)).error(),
              fiber::common::IoErr::Invalid);
    // Truncated armor.
    const std::string truncated(certfix::kRsa2048KeyPem, std::strlen(certfix::kRsa2048KeyPem) / 2);
    EXPECT_EQ(TlsPrivateKey::parse_pem({truncated.data(), truncated.size()}).error(), fiber::common::IoErr::Invalid);

    const std::uint8_t garbage[] = {0xde, 0xad, 0xbe, 0xef};
    EXPECT_EQ(TlsPrivateKey::parse_der(garbage).error(), fiber::common::IoErr::Invalid);
    EXPECT_EQ(TlsPrivateKey::parse_der({}).error(), fiber::common::IoErr::Invalid);
}

TEST(TlsPrivateKeyParse, MoveTransfersOwnership) {
    auto a = TlsPrivateKey::parse_pem(pem_span(certfix::kRsa2048KeyPem));
    ASSERT_TRUE(a.has_value());
    TlsPrivateKey moved = std::move(*a);
    EXPECT_TRUE(a->empty());
    EXPECT_FALSE(moved.empty());
    EXPECT_EQ(TlsKeyKind::Rsa, moved.key_kind());
    // The moved-to key still signs.
    std::array<std::uint8_t, 512> out{};
    auto len = moved.sign(TlsSignatureScheme::RsaPssRsaeSha256, TlsProtocolVersion::Tls13, as_span(sigvec::kContentA),
                          out);
    ASSERT_TRUE(len.has_value());
    EXPECT_GT(*len, 0u);
}

TEST(TlsPrivateKeyApi, MaxSignatureLen) {
    auto rsa = TlsPrivateKey::parse_pem(pem_span(certfix::kRsa2048KeyPem));
    ASSERT_TRUE(rsa.has_value());
    EXPECT_EQ(256u, rsa->max_signature_len()); // 2048-bit modulus
    auto ed = TlsPrivateKey::parse_pem(pem_span(certfix::kEd25519KeyPem));
    ASSERT_TRUE(ed.has_value());
    EXPECT_EQ(64u, ed->max_signature_len());
}

// ---------------------------------------------------------------------------
// supports() matrix (02b §3.2) — the hand-pinned oracle
// ---------------------------------------------------------------------------

constexpr TlsSignatureScheme kSchemeOrder[] = {
        TlsSignatureScheme::RsaPkcs1Sha256, // 0
        TlsSignatureScheme::RsaPkcs1Sha384, // 1
        TlsSignatureScheme::RsaPkcs1Sha512, // 2
        TlsSignatureScheme::EcdsaSecp256r1Sha256, // 3
        TlsSignatureScheme::EcdsaSecp384r1Sha384, // 4
        TlsSignatureScheme::EcdsaSecp521r1Sha512, // 5
        TlsSignatureScheme::Ed25519, // 6
        TlsSignatureScheme::RsaPssRsaeSha256, // 7
        TlsSignatureScheme::RsaPssRsaeSha384, // 8
        TlsSignatureScheme::RsaPssRsaeSha512, // 9
};

struct KeySupport {
    const char *key_pem;
    const char *cert_pem; // public half, for the view side of the matrix
    TlsKeyKind kind;
    bool tls12[10];
    bool tls13[10];
};

// Pinned expectations, mirroring ssl_pkey_supports_algorithm: rsa_pkcs1_* is
// TLS 1.2-only; RSA-PSS needs modulus >= 2*hash+2 (1024-bit fails SHA-512);
// TLS 1.3 requires the scheme's exact EC curve while TLS 1.2 accepts any EC
// curve for any ECDSA scheme (the 1.2 scheme id carries no curve semantics).
const KeySupport kKeyMatrix[] = {
        {certfix::kRsa2048KeyPem,
         certfix::kLeafRsaPem,
         TlsKeyKind::Rsa,
         {true, true, true, false, false, false, false, true, true, true},
         {false, false, false, false, false, false, false, true, true, true}},
        {certfix::kRsa1024KeyPem,
         certfix::kCertRsa1024Pem,
         TlsKeyKind::Rsa,
         {true, true, true, false, false, false, false, true, true, false},
         {false, false, false, false, false, false, false, true, true, false}},
        {certfix::kP256KeyPem,
         certfix::kLeafEcP256Pem,
         TlsKeyKind::Ec,
         {false, false, false, true, true, true, false, false, false, false},
         {false, false, false, true, false, false, false, false, false, false}},
        {certfix::kP384KeyPem,
         certfix::kLeafEcP384Pem,
         TlsKeyKind::Ec,
         {false, false, false, true, true, true, false, false, false, false},
         {false, false, false, false, true, false, false, false, false, false}},
        {certfix::kP521KeyPem,
         certfix::kCertP521Pem,
         TlsKeyKind::Ec,
         {false, false, false, true, true, true, false, false, false, false},
         {false, false, false, false, false, true, false, false, false, false}},
        {certfix::kEd25519KeyPem,
         certfix::kLeafEd25519Pem,
         TlsKeyKind::Ed25519,
         {false, false, false, false, false, false, true, false, false, false},
         {false, false, false, false, false, false, true, false, false, false}},
};

TEST(TlsSignatureSupport, SchemeKeyMatrix) {
    for (const KeySupport &row: kKeyMatrix) {
        auto key = TlsPrivateKey::parse_pem(pem_span(row.key_pem));
        ASSERT_TRUE(key.has_value());
        EXPECT_EQ(row.kind, key->key_kind());
        TlsCertificateChain holder;
        const TlsPublicKeyView view = cert_public_key(row.cert_pem, holder);
        ASSERT_FALSE(view.empty());
        for (std::size_t i = 0; i < std::size(kSchemeOrder); ++i) {
            const char *name = scheme_name(kSchemeOrder[i]);
            EXPECT_EQ(row.tls12[i], key->supports(kSchemeOrder[i], TlsProtocolVersion::Tls12))
                    << name << " tls12 (private)";
            EXPECT_EQ(row.tls13[i], key->supports(kSchemeOrder[i], TlsProtocolVersion::Tls13))
                    << name << " tls13 (private)";
            EXPECT_EQ(row.tls12[i], view.supports(kSchemeOrder[i], TlsProtocolVersion::Tls12))
                    << name << " tls12 (public view)";
            EXPECT_EQ(row.tls13[i], view.supports(kSchemeOrder[i], TlsProtocolVersion::Tls13))
                    << name << " tls13 (public view)";
        }
    }
}

TEST(TlsSignatureSupport, DefaultViewSupportsNothing) {
    const TlsPublicKeyView view;
    EXPECT_TRUE(view.empty());
    for (std::size_t i = 0; i < std::size(kSchemeOrder); ++i) {
        EXPECT_FALSE(view.supports(kSchemeOrder[i], TlsProtocolVersion::Tls12));
        EXPECT_FALSE(view.supports(kSchemeOrder[i], TlsProtocolVersion::Tls13));
    }
    // Verify with the empty view reports the mismatch instead of asserting.
    auto res = tls_verify(TlsSignatureScheme::RsaPssRsaeSha256, TlsProtocolVersion::Tls13, view,
                          as_span(sigvec::kContentB), {});
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(fiber::common::IoErr::Invalid, res.error());
}

// ---------------------------------------------------------------------------
// KAT vectors
// ---------------------------------------------------------------------------

TEST(TlsSignatureKat, FixedVectorsVerify) {
    for (const sigvec::KatVector &vec: sigvec::kVectors) {
        TlsCertificateChain holder;
        const TlsPublicKeyView view = cert_public_key(vec.cert_pem, holder);
        ASSERT_FALSE(view.empty());
        auto ok = tls_verify(vec.scheme, vec.version, view, vec.content, vec.signature);
        ASSERT_TRUE(ok.has_value()) << scheme_name(vec.scheme);
        EXPECT_TRUE(*ok) << scheme_name(vec.scheme);
    }
}

TEST(TlsSignatureKat, TamperedAndTruncatedSignaturesFail) {
    for (const sigvec::KatVector &vec: sigvec::kVectors) {
        TlsCertificateChain holder;
        const TlsPublicKeyView view = cert_public_key(vec.cert_pem, holder);
        ASSERT_FALSE(view.empty());

        std::vector<std::uint8_t> bad(vec.signature.begin(), vec.signature.end());
        ASSERT_FALSE(bad.empty());
        bad[bad.size() / 2] ^= 0xA5;
        auto tampered = tls_verify(vec.scheme, vec.version, view, vec.content, bad);
        ASSERT_TRUE(tampered.has_value()) << scheme_name(vec.scheme);
        EXPECT_FALSE(*tampered) << scheme_name(vec.scheme);

        auto truncated = tls_verify(vec.scheme, vec.version, view, vec.content, {bad.data(), bad.size() - 1});
        ASSERT_TRUE(truncated.has_value()) << scheme_name(vec.scheme);
        EXPECT_FALSE(*truncated) << scheme_name(vec.scheme);
    }
}

TEST(TlsSignatureKat, SchemeNotMatchingKeyIsInvalid) {
    // RSA signature (kVectors[0] = rsa_pkcs1_sha256/TLS 1.2) under an EC cert.
    const sigvec::KatVector &rsa = sigvec::kVectors[0];
    TlsCertificateChain ec_holder;
    const TlsPublicKeyView ec = cert_public_key(certfix::kLeafEcP256Pem, ec_holder);
    auto res = tls_verify(rsa.scheme, rsa.version, ec, rsa.content, rsa.signature);
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(fiber::common::IoErr::Invalid, res.error());

    // ECDSA signature under an RSA cert.
    const sigvec::KatVector *ecdsa = nullptr;
    const sigvec::KatVector *ed25519 = nullptr;
    for (const sigvec::KatVector &vec: sigvec::kVectors) {
        if (ecdsa == nullptr && vec.scheme == TlsSignatureScheme::EcdsaSecp256r1Sha256 &&
            vec.version == TlsProtocolVersion::Tls13) {
            ecdsa = &vec;
        }
        if (ed25519 == nullptr && vec.scheme == TlsSignatureScheme::Ed25519) {
            ed25519 = &vec;
        }
    }
    ASSERT_NE(nullptr, ecdsa);
    ASSERT_NE(nullptr, ed25519);

    TlsCertificateChain rsa_holder;
    const TlsPublicKeyView rsa_cert = cert_public_key(certfix::kLeafRsaPem, rsa_holder);
    auto res2 = tls_verify(ecdsa->scheme, ecdsa->version, rsa_cert, ecdsa->content, ecdsa->signature);
    ASSERT_FALSE(res2.has_value());
    EXPECT_EQ(fiber::common::IoErr::Invalid, res2.error());

    // Ed25519 signature under the same EC cert.
    auto res3 = tls_verify(ed25519->scheme, ed25519->version, ec, ed25519->content, ed25519->signature);
    ASSERT_FALSE(res3.has_value());
    EXPECT_EQ(fiber::common::IoErr::Invalid, res3.error());
}

TEST(TlsSignatureKat, SignatureUnderWrongKeyOfSameFamilyFails) {
    // kVectors[0] was signed by rsa2048; cert_rsa1024 holds a different RSA
    // key, so supports() passes and the failure is a genuine verify false.
    const sigvec::KatVector &vec = sigvec::kVectors[0];
    TlsCertificateChain other_holder;
    const TlsPublicKeyView other = cert_public_key(certfix::kCertRsa1024Pem, other_holder);
    auto res = tls_verify(vec.scheme, vec.version, other, vec.content, vec.signature);
    ASSERT_TRUE(res.has_value());
    EXPECT_FALSE(*res);
}

// ---------------------------------------------------------------------------
// Round trip
// ---------------------------------------------------------------------------

TEST(TlsSignatureRoundtrip, SignThenVerifyEverySupportedPair) {
    for (const KeySupport &row: kKeyMatrix) {
        auto key = TlsPrivateKey::parse_pem(pem_span(row.key_pem));
        ASSERT_TRUE(key.has_value());
        TlsCertificateChain holder;
        const TlsPublicKeyView view = cert_public_key(row.cert_pem, holder);
        ASSERT_FALSE(view.empty());

        std::array<std::uint8_t, 512> out{};
        for (std::size_t i = 0; i < std::size(kSchemeOrder); ++i) {
            const TlsSignatureScheme scheme = kSchemeOrder[i];
            for (const TlsProtocolVersion version: {TlsProtocolVersion::Tls12, TlsProtocolVersion::Tls13}) {
                if (!key->supports(scheme, version)) {
                    continue;
                }
                auto len = key->sign(scheme, version, as_span(sigvec::kContentA), out);
                ASSERT_TRUE(len.has_value()) << scheme_name(scheme);
                EXPECT_LE(*len, key->max_signature_len()) << scheme_name(scheme);
                auto ok = tls_verify(scheme, version, view, as_span(sigvec::kContentA), {out.data(), *len});
                ASSERT_TRUE(ok.has_value()) << scheme_name(scheme);
                EXPECT_TRUE(*ok) << scheme_name(scheme);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Contract violations are engine bugs — FIBER_ASSERT
// ---------------------------------------------------------------------------

// The static-RSA ClientKeyExchange primitive (feature/tls/11): PKCS#1 v1.5
// encryption to a certificate's RSA key decrypts under the paired private
// key; a non-RSA key or a short output buffer is Invalid.
TEST(TlsRsaEncrypt, Pkcs1RoundTripsUnderThePairedKey) {
    TlsCertificateChain rsa_holder;
    const TlsPublicKeyView rsa_cert = cert_public_key(certfix::kLeafRsaPem, rsa_holder);
    auto private_key = TlsPrivateKey::parse_pem(pem_span(certfix::kRsa2048KeyPem));
    ASSERT_TRUE(private_key.has_value());

    std::array<std::uint8_t, 48> premaster{};
    for (std::size_t i = 0; i < premaster.size(); ++i) {
        premaster[i] = static_cast<std::uint8_t>(0x03 + i);
    }
    std::array<std::uint8_t, 512> encrypted{};
    const auto len = rsa_cert.rsa_encrypt_pkcs1(premaster, encrypted);
    ASSERT_TRUE(len.has_value());
    EXPECT_EQ(256u, len.value()); // the RSA-2048 modulus

    RSA *rsa = EVP_PKEY_get0_RSA(static_cast<EVP_PKEY *>(private_key->evp_pkey_handle()));
    ASSERT_NE(nullptr, rsa);
    std::array<std::uint8_t, 256> decrypted{};
    std::size_t decrypted_len = 0;
    ASSERT_EQ(1, RSA_decrypt(rsa, &decrypted_len, decrypted.data(), decrypted.size(), encrypted.data(), len.value(),
                             RSA_PKCS1_PADDING));
    ASSERT_EQ(premaster.size(), decrypted_len);
    EXPECT_EQ(0, std::memcmp(premaster.data(), decrypted.data(), premaster.size()));

    // PKCS#1 v1.5 padding is randomized: the same premaster never encrypts twice alike.
    std::array<std::uint8_t, 512> again{};
    ASSERT_TRUE(rsa_cert.rsa_encrypt_pkcs1(premaster, again).has_value());
    EXPECT_NE(0, std::memcmp(encrypted.data(), again.data(), 256));

    std::array<std::uint8_t, 255> short_out{};
    EXPECT_FALSE(rsa_cert.rsa_encrypt_pkcs1(premaster, short_out).has_value());

    TlsCertificateChain ec_holder;
    const TlsPublicKeyView ec_cert = cert_public_key(certfix::kLeafEcP256Pem, ec_holder);
    EXPECT_FALSE(ec_cert.rsa_encrypt_pkcs1(premaster, encrypted).has_value());
}

TEST(TlsSignatureDeath, ContractViolations) {
    auto key = TlsPrivateKey::parse_pem(pem_span(certfix::kRsa2048KeyPem));
    ASSERT_TRUE(key.has_value());
    std::array<std::uint8_t, 512> out{};

    // Scheme the key cannot produce at all.
    EXPECT_DEATH(
            (void) key->sign(TlsSignatureScheme::Ed25519, TlsProtocolVersion::Tls13, as_span(sigvec::kContentA), out),
            "FIBER_ASSERT failed");
    // rsa_pkcs1_* never signs a 1.3 handshake (version gate).
    EXPECT_DEATH((void) key->sign(TlsSignatureScheme::RsaPkcs1Sha256, TlsProtocolVersion::Tls13,
                                  as_span(sigvec::kContentA), out),
                 "FIBER_ASSERT failed");
    // Unloaded key.
    const TlsPrivateKey empty_key;
    EXPECT_DEATH((void) empty_key.sign(TlsSignatureScheme::RsaPssRsaeSha256, TlsProtocolVersion::Tls12,
                                       as_span(sigvec::kContentA), out),
                 "FIBER_ASSERT failed");
}
