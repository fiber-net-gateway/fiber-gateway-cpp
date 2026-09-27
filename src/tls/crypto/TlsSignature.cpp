#include <fiber/tls/crypto/TlsSignature.h>

// BoringSSL adapter for the TLS signature primitives (feature/tls/02b §3).
// The scheme table and the DigestSign setup mirror BoringSSL's own
// ssl_privkey.cc so the behavior (PSS salt length, Ed25519 NULL digest, the
// TLS 1.3 EC curve gate, the RSA-PSS modulus bound) is defined by reference
// implementation, not by us.

#include <openssl/digest.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/rsa.h>

#include <iterator>

#include <fiber/common/Assert.h>
#include <fiber/common/util/Base64.h>

namespace fiber::tls {
namespace {

// One row per implemented scheme (02b §3.2). digest == nullptr means
// Ed25519's raw-input mode (EVP_DigestSignInit with a NULL digest).
struct SchemeInfo {
    TlsSignatureScheme scheme;
    int pkey_type; // EVP_PKEY_RSA | EVP_PKEY_EC | EVP_PKEY_ED25519
    int curve_nid; // checked in TLS 1.3 only; 0 = any/no curve constraint
    const EVP_MD *(*digest)(void);
    bool is_rsa_pss;
    bool tls12_ok;
    bool tls13_ok;
};

constexpr SchemeInfo kSchemeTable[] = {
        // rsa_pkcs1_*: TLS 1.2 only (never signs a 1.3 handshake).
        {TlsSignatureScheme::RsaPkcs1Sha256, EVP_PKEY_RSA, 0, &EVP_sha256, false, true, false},
        {TlsSignatureScheme::RsaPkcs1Sha384, EVP_PKEY_RSA, 0, &EVP_sha384, false, true, false},
        {TlsSignatureScheme::RsaPkcs1Sha512, EVP_PKEY_RSA, 0, &EVP_sha512, false, true, false},
        {TlsSignatureScheme::EcdsaSecp256r1Sha256, EVP_PKEY_EC, NID_X9_62_prime256v1, &EVP_sha256, false, true, true},
        {TlsSignatureScheme::EcdsaSecp384r1Sha384, EVP_PKEY_EC, NID_secp384r1, &EVP_sha384, false, true, true},
        {TlsSignatureScheme::EcdsaSecp521r1Sha512, EVP_PKEY_EC, NID_secp521r1, &EVP_sha512, false, true, true},
        {TlsSignatureScheme::Ed25519, EVP_PKEY_ED25519, 0, nullptr, false, true, true},
        {TlsSignatureScheme::RsaPssRsaeSha256, EVP_PKEY_RSA, 0, &EVP_sha256, true, true, true},
        {TlsSignatureScheme::RsaPssRsaeSha384, EVP_PKEY_RSA, 0, &EVP_sha384, true, true, true},
        {TlsSignatureScheme::RsaPssRsaeSha512, EVP_PKEY_RSA, 0, &EVP_sha512, true, true, true},
};

const SchemeInfo *scheme_info(TlsSignatureScheme scheme) noexcept {
    for (const SchemeInfo &info: kSchemeTable) {
        if (info.scheme == scheme) {
            return &info;
        }
    }
    return nullptr;
}

// The shared matching core behind TlsPrivateKey::supports and
// TlsPublicKeyView::supports (ssl_pkey_supports_algorithm semantics for the
// ten in-scope schemes: pkey type, PSS modulus bound, the TLS 1.3 EC curve
// NID, version gates). TLS 1.2 deliberately does not check the curve — the
// 1.2 scheme carries no curve semantics and the peer owns that choice.
bool pkey_supports(const void *impl, TlsSignatureScheme scheme, TlsProtocolVersion version) noexcept {
    if (impl == nullptr) {
        return false;
    }
    const auto *pkey = static_cast<const EVP_PKEY *>(impl);
    const SchemeInfo *info = scheme_info(scheme);
    if (info == nullptr || EVP_PKEY_id(pkey) != info->pkey_type) {
        return false;
    }
    // RSASSA-PSS needs emLen >= hLen + sLen + 2; both are the hash size in
    // TLS, so 1024-bit RSA is too small for SHA-512 (the reason this check
    // exists at all — test credentials).
    if (info->is_rsa_pss && static_cast<std::size_t>(EVP_PKEY_size(pkey)) < 2u * EVP_MD_size(info->digest()) + 2u) {
        return false;
    }
    if (version >= TlsProtocolVersion::Tls13) {
        if (!info->tls13_ok) {
            return false;
        }
        if (info->pkey_type == EVP_PKEY_EC &&
            (info->curve_nid == 0 || EVP_PKEY_get_ec_curve_nid(pkey) != info->curve_nid)) {
            return false;
        }
    } else if (!info->tls12_ok) {
        return false;
    }
    return true;
}

TlsKeyKind pkey_kind(const void *impl) noexcept {
    const auto *pkey = static_cast<const EVP_PKEY *>(impl);
    switch (EVP_PKEY_id(pkey)) {
        case EVP_PKEY_RSA:
            return TlsKeyKind::Rsa;
        case EVP_PKEY_EC:
            return TlsKeyKind::Ec;
        case EVP_PKEY_ED25519:
            return TlsKeyKind::Ed25519;
        default:
            FIBER_PANIC("unsupported private key type");
    }
}

// PEM block extraction without BIO: finds the first "-----BEGIN <LABEL>-----"
// ... "-----END <LABEL>-----" run among |labels|, gathering the base64 body
// (whitespace skipped) and decoding it. Configuration-path code — the
// std::string scratch is fine here (once per credential, not per handshake).
common::IoResult<std::string> extract_pem_block(std::span<const char> pem, const char *const *labels,
                                                std::size_t num_labels) {
    const std::string_view text(pem.data(), pem.size());
    for (std::size_t i = 0; i < num_labels; ++i) {
        const std::string begin = std::string("-----BEGIN ") + labels[i] + "-----";
        const std::string end = std::string("-----END ") + labels[i] + "-----";
        const std::size_t begin_pos = text.find(begin);
        if (begin_pos == std::string_view::npos) {
            continue;
        }
        const std::size_t body_start = begin_pos + begin.size();
        const std::size_t end_pos = text.find(end, body_start);
        if (end_pos == std::string_view::npos) {
            return std::unexpected(common::IoErr::Invalid);
        }
        // Reject encrypted PEM ("Proc-Type: 4,ENCRYPTED" header lines) before
        // squashing whitespace: an encrypted body would otherwise surface as
        // an opaque DER failure.
        std::string header(text.substr(body_start, std::min<std::size_t>(64, end_pos - body_start)));
        if (header.find("Proc-Type") != std::string::npos) {
            return std::unexpected(common::IoErr::Invalid);
        }
        std::string b64;
        b64.reserve(end_pos - body_start);
        for (std::size_t j = body_start; j < end_pos; ++j) {
            const char c = text[j];
            if (c == '\n' || c == '\r' || c == ' ' || c == '\t') {
                continue;
            }
            b64.push_back(c);
        }
        std::string der;
        if (!util::base64_decode(b64, der)) {
            return std::unexpected(common::IoErr::Invalid);
        }
        return der;
    }
    return std::unexpected(common::IoErr::Invalid);
}

} // namespace

TlsKeyKind TlsPublicKeyView::key_kind() const noexcept {
    FIBER_ASSERT(impl_ != nullptr);
    return pkey_kind(impl_);
}

bool TlsPublicKeyView::supports(TlsSignatureScheme scheme, TlsProtocolVersion version) const noexcept {
    return pkey_supports(impl_, scheme, version);
}

common::IoResult<std::size_t> TlsPublicKeyView::rsa_encrypt_pkcs1(std::span<const std::uint8_t> in,
                                                                  std::span<std::uint8_t> out) const noexcept {
    FIBER_ASSERT(impl_ != nullptr);
    RSA *rsa = EVP_PKEY_get0_RSA(static_cast<const EVP_PKEY *>(impl_));
    if (rsa == nullptr || out.size() < RSA_size(rsa)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    std::size_t out_len = 0;
    if (RSA_encrypt(rsa, &out_len, out.data(), out.size(), in.data(), in.size(), RSA_PKCS1_PADDING) != 1) {
        ERR_clear_error();
        return std::unexpected(common::IoErr::Invalid);
    }
    return out_len;
}

TlsPrivateKey::TlsPrivateKey(TlsPrivateKey &&other) noexcept : impl_(other.impl_) { other.impl_ = nullptr; }

TlsPrivateKey &TlsPrivateKey::operator=(TlsPrivateKey &&other) noexcept {
    if (this != &other) {
        if (impl_ != nullptr) {
            EVP_PKEY_free(static_cast<EVP_PKEY *>(impl_));
        }
        impl_ = other.impl_;
        other.impl_ = nullptr;
    }
    return *this;
}

TlsPrivateKey::~TlsPrivateKey() {
    if (impl_ != nullptr) {
        EVP_PKEY_free(static_cast<EVP_PKEY *>(impl_));
    }
}

common::IoResult<TlsPrivateKey> TlsPrivateKey::parse_der(std::span<const std::uint8_t> der) noexcept {
    if (der.empty()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    const std::uint8_t *cursor = der.data();
    EVP_PKEY *pkey = d2i_AutoPrivateKey(nullptr, &cursor, static_cast<long>(der.size()));
    if (pkey == nullptr) {
        ERR_clear_error();
        return std::unexpected(common::IoErr::Invalid);
    }
    TlsPrivateKey key;
    key.impl_ = pkey;
    return key;
}

common::IoResult<TlsPrivateKey> TlsPrivateKey::parse_pem(std::span<const char> pem) noexcept {
    static const char *const kLabels[] = {"PRIVATE KEY", "RSA PRIVATE KEY", "EC PRIVATE KEY"};
    auto der = extract_pem_block(pem, kLabels, std::size(kLabels));
    if (!der.has_value()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return parse_der({reinterpret_cast<const std::uint8_t *>(der->data()), der->size()});
}

TlsKeyKind TlsPrivateKey::key_kind() const noexcept {
    FIBER_ASSERT(impl_ != nullptr);
    return pkey_kind(impl_);
}

bool TlsPrivateKey::supports(TlsSignatureScheme scheme, TlsProtocolVersion version) const noexcept {
    return pkey_supports(impl_, scheme, version);
}

std::size_t TlsPrivateKey::max_signature_len() const noexcept {
    FIBER_ASSERT(impl_ != nullptr);
    return static_cast<std::size_t>(EVP_PKEY_size(static_cast<EVP_PKEY *>(impl_)));
}

common::IoResult<std::size_t> TlsPrivateKey::sign(TlsSignatureScheme scheme, TlsProtocolVersion version,
                                                  std::span<const std::uint8_t> content,
                                                  std::span<std::uint8_t> out) const noexcept {
    FIBER_ASSERT(impl_ != nullptr);
    // The engine selected the scheme through supports(); a mismatch here is
    // an engine bug, not a protocol event.
    FIBER_ASSERT(pkey_supports(impl_, scheme, version));

    const SchemeInfo *info = scheme_info(scheme);
    EVP_MD_CTX ctx;
    EVP_MD_CTX_init(&ctx);
    EVP_PKEY_CTX *pctx = nullptr;
    const EVP_MD *digest = info->digest != nullptr ? info->digest() : nullptr;
    if (EVP_DigestSignInit(&ctx, &pctx, digest, nullptr, static_cast<EVP_PKEY *>(impl_)) != 1 ||
        (info->is_rsa_pss && (EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) != 1 ||
                              EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, RSA_PSS_SALTLEN_DIGEST) != 1))) {
        EVP_MD_CTX_cleanup(&ctx);
        ERR_clear_error();
        return std::unexpected(common::IoErr::Unknown); // unreachable given supports()
    }
    std::size_t sig_len = out.size();
    const int ok = EVP_DigestSign(&ctx, out.data(), &sig_len, content.data(), content.size());
    EVP_MD_CTX_cleanup(&ctx);
    if (ok != 1) {
        ERR_clear_error();
        return std::unexpected(common::IoErr::Unknown);
    }
    return sig_len;
}

common::IoResult<bool> tls_verify(TlsSignatureScheme scheme, TlsProtocolVersion version,
                                  const TlsPublicKeyView &public_key, std::span<const std::uint8_t> content,
                                  std::span<const std::uint8_t> signature) noexcept {
    if (!pkey_supports(public_key.impl_, scheme, version)) {
        // The peer picked a scheme that cannot match the certificate's key:
        // illegal_parameter, not decrypt_error.
        return std::unexpected(common::IoErr::Invalid);
    }
    FIBER_ASSERT(public_key.impl_ != nullptr);

    const SchemeInfo *info = scheme_info(scheme);
    EVP_MD_CTX ctx;
    EVP_MD_CTX_init(&ctx);
    EVP_PKEY_CTX *pctx = nullptr;
    const EVP_MD *digest = info->digest != nullptr ? info->digest() : nullptr;
    if (EVP_DigestVerifyInit(&ctx, &pctx, digest, nullptr,
                             const_cast<EVP_PKEY *>(static_cast<const EVP_PKEY *>(public_key.impl_))) != 1 ||
        (info->is_rsa_pss && (EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) != 1 ||
                              EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, RSA_PSS_SALTLEN_DIGEST) != 1))) {
        EVP_MD_CTX_cleanup(&ctx);
        ERR_clear_error();
        return std::unexpected(common::IoErr::Unknown);
    }
    const int ok = EVP_DigestVerify(&ctx, signature.data(), signature.size(), content.data(), content.size());
    EVP_MD_CTX_cleanup(&ctx);
    ERR_clear_error();
    // 1 = valid; 0 = wrong signature OR malformed signature encoding — both
    // are the same protocol event for the engine (decrypt_error).
    return ok == 1;
}

} // namespace fiber::tls
