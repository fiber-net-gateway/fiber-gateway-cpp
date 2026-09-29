#include <fiber/tls/crypto/TlsCertificate.h>

// BoringSSL adapter for certificate material (feature/tls/02b §4). Parsing
// goes CRYPTO_BUFFER -> X509_parse_from_buffer (no BIO anywhere); the alert
// mapping mirrors BoringSSL's SSL_alert_from_verify_result (ssl_x509.cc); the
// purpose (EKU) check mirrors its X509_STORE_CTX_set_default("ssl_client"/
// "ssl_server") through X509_VERIFY_PARAM_set_purpose.

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pool.h>
#include <openssl/stack.h>
#include <openssl/x509.h>

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include <unistd.h>

#include <fiber/common/Assert.h>
#include <fiber/common/util/Base64.h>

namespace fiber::tls {
namespace {

// ---- verify-error -> alert (02b §4.5, from SSL_alert_from_verify_result) ----

TlsAlertDesc alert_from_verify_error(int err) noexcept {
    switch (err) {
        case X509_V_ERR_CERT_CHAIN_TOO_LONG:
        case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT:
        case X509_V_ERR_INVALID_CA:
        case X509_V_ERR_PATH_LENGTH_EXCEEDED:
        case X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN:
        case X509_V_ERR_UNABLE_TO_GET_CRL:
        case X509_V_ERR_UNABLE_TO_GET_CRL_ISSUER:
        case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT:
        case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY:
        case X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE:
            return TlsAlertDesc::UnknownCa;

        case X509_V_ERR_UNABLE_TO_DECRYPT_CERT_SIGNATURE:
        case X509_V_ERR_UNABLE_TO_DECRYPT_CRL_SIGNATURE:
        case X509_V_ERR_UNABLE_TO_DECODE_ISSUER_PUBLIC_KEY:
        case X509_V_ERR_ERROR_IN_CERT_NOT_BEFORE_FIELD:
        case X509_V_ERR_ERROR_IN_CERT_NOT_AFTER_FIELD:
        case X509_V_ERR_ERROR_IN_CRL_LAST_UPDATE_FIELD:
        case X509_V_ERR_ERROR_IN_CRL_NEXT_UPDATE_FIELD:
        case X509_V_ERR_CERT_UNTRUSTED:
        case X509_V_ERR_CERT_REJECTED:
        case X509_V_ERR_HOSTNAME_MISMATCH:
        case X509_V_ERR_EMAIL_MISMATCH:
        case X509_V_ERR_IP_ADDRESS_MISMATCH:
            return TlsAlertDesc::BadCertificate;

        case X509_V_ERR_CERT_SIGNATURE_FAILURE:
        case X509_V_ERR_CRL_SIGNATURE_FAILURE:
            return TlsAlertDesc::DecryptError;

        case X509_V_ERR_CERT_HAS_EXPIRED:
        case X509_V_ERR_CERT_NOT_YET_VALID:
        case X509_V_ERR_CRL_HAS_EXPIRED:
        case X509_V_ERR_CRL_NOT_YET_VALID:
            return TlsAlertDesc::CertificateExpired;

        case X509_V_ERR_CERT_REVOKED:
            return TlsAlertDesc::CertificateRevoked;

        case X509_V_ERR_UNSPECIFIED:
        case X509_V_ERR_OUT_OF_MEM:
        case X509_V_ERR_INVALID_CALL:
        case X509_V_ERR_STORE_LOOKUP:
            return TlsAlertDesc::InternalError;

        case X509_V_ERR_APPLICATION_VERIFICATION:
            return TlsAlertDesc::HandshakeFailure;

        case X509_V_ERR_INVALID_PURPOSE:
            return TlsAlertDesc::UnsupportedCertificate;

        default:
            return TlsAlertDesc::CertificateUnknown;
    }
}

// Extracts every "-----BEGIN CERTIFICATE-----" block's DER from a PEM bundle.
// max_blocks is the caller's ceiling: leaf chains cap at kMaxCerts; trust
// stores take system root bundles (hundreds). Configuration-path code; the
// std::string scratch runs once per load.
common::IoResult<std::vector<std::string>> decode_pem_certificates(std::span<const char> pem, std::size_t max_blocks) {
    const std::string_view text(pem.data(), pem.size());
    constexpr std::string_view kBegin = "-----BEGIN CERTIFICATE-----";
    constexpr std::string_view kEnd = "-----END CERTIFICATE-----";
    std::vector<std::string> ders;
    std::size_t pos = 0;
    while (true) {
        const std::size_t begin = text.find(kBegin, pos);
        if (begin == std::string_view::npos) {
            break;
        }
        const std::size_t body = begin + kBegin.size();
        const std::size_t end = text.find(kEnd, body);
        if (end == std::string_view::npos) {
            return std::unexpected(common::IoErr::Invalid);
        }
        std::string b64;
        b64.reserve(end - body);
        for (std::size_t i = body; i < end; ++i) {
            const char c = text[i];
            if (c == '\n' || c == '\r' || c == ' ' || c == '\t') {
                continue;
            }
            b64.push_back(c);
        }
        std::string der;
        if (!util::base64_decode(b64, der)) {
            return std::unexpected(common::IoErr::Invalid);
        }
        if (ders.size() >= max_blocks) {
            return std::unexpected(common::IoErr::MessageTooLarge);
        }
        ders.push_back(std::move(der));
        pos = end + kEnd.size();
    }
    if (ders.empty()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return ders;
}

// One-line X509_NAME rendering (X509_NAME_oneline needs no BIO). A buffer
// too small for the first entry truncates to an empty line — BoringSSL stops
// at the last complete entry rather than failing — so callers pass a
// log-line-sized buffer; Invalid covers only the degenerate inputs.
common::IoResult<std::size_t> name_line(const X509_NAME *name, std::span<char> out) noexcept {
    if (name == nullptr || out.empty() || out.size() > static_cast<std::size_t>(INT_MAX)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (X509_NAME_oneline(const_cast<X509_NAME *>(name), out.data(), static_cast<int>(out.size())) == nullptr) {
        ERR_clear_error();
        return std::unexpected(common::IoErr::Invalid);
    }
    return std::strlen(out.data());
}

// Shared parse core: DER -> (X509, CRYPTO_BUFFER), each an owning reference
// (X509_parse_from_buffer up-refs the buffer). Callers either wrap the pair
// in a TlsCertificate or hand the X509 to a store and free both.
struct RawCert {
    X509 *x509 = nullptr;
    CRYPTO_BUFFER *buf = nullptr;
};

common::IoResult<RawCert> parse_raw(std::span<const std::uint8_t> der) noexcept {
    if (der.empty() || der.size() > TlsCertificate::kMaxDerLen) {
        return std::unexpected(common::IoErr::Invalid);
    }
    CRYPTO_BUFFER *buf = CRYPTO_BUFFER_new(der.data(), der.size(), nullptr);
    if (buf == nullptr) {
        ERR_clear_error();
        return std::unexpected(common::IoErr::NoMem);
    }
    X509 *x509 = X509_parse_from_buffer(buf);
    if (x509 == nullptr) {
        ERR_clear_error();
        CRYPTO_BUFFER_free(buf);
        return std::unexpected(common::IoErr::Invalid);
    }
    return RawCert{x509, buf};
}

void raw_cert_free(RawCert &raw) noexcept {
    X509_free(raw.x509);
    CRYPTO_BUFFER_free(raw.buf);
    raw.x509 = nullptr;
    raw.buf = nullptr;
}

} // namespace

// ---- TlsCertificate ----

TlsCertificate::TlsCertificate(TlsCertificate &&other) noexcept :
    x509_(other.x509_), der_buf_(other.der_buf_), der_len_(other.der_len_) {
    other.x509_ = nullptr;
    other.der_buf_ = nullptr;
    other.der_len_ = 0;
}

TlsCertificate &TlsCertificate::operator=(TlsCertificate &&other) noexcept {
    if (this != &other) {
        this->~TlsCertificate();
        x509_ = other.x509_;
        der_buf_ = other.der_buf_;
        der_len_ = other.der_len_;
        other.x509_ = nullptr;
        other.der_buf_ = nullptr;
        other.der_len_ = 0;
    }
    return *this;
}

TlsCertificate::~TlsCertificate() {
    if (x509_ != nullptr) {
        X509_free(static_cast<X509 *>(x509_));
    }
    if (der_buf_ != nullptr) {
        CRYPTO_BUFFER_free(static_cast<CRYPTO_BUFFER *>(der_buf_));
    }
}

common::IoResult<TlsCertificate> TlsCertificate::parse_der(std::span<const std::uint8_t> der) noexcept {
    auto raw = parse_raw(der);
    if (!raw.has_value()) {
        return std::unexpected(raw.error());
    }
    TlsCertificate cert;
    cert.x509_ = raw->x509;
    cert.der_buf_ = raw->buf;
    cert.der_len_ = static_cast<std::uint32_t>(der.size());
    return cert;
}

std::span<const std::uint8_t> TlsCertificate::der() const noexcept {
    FIBER_ASSERT(der_buf_ != nullptr);
    return {static_cast<const std::uint8_t *>(CRYPTO_BUFFER_data(static_cast<CRYPTO_BUFFER *>(der_buf_))), der_len_};
}

common::IoResult<TlsPublicKeyView> TlsCertificate::public_key() const noexcept {
    FIBER_ASSERT(x509_ != nullptr);
    EVP_PKEY *pkey = X509_get0_pubkey(static_cast<X509 *>(x509_));
    if (pkey == nullptr) {
        ERR_clear_error();
        return std::unexpected(common::IoErr::Invalid);
    }
    switch (EVP_PKEY_id(pkey)) {
        case EVP_PKEY_RSA:
        case EVP_PKEY_EC:
        case EVP_PKEY_ED25519:
            return TlsPublicKeyView(pkey);
        default:
            return std::unexpected(common::IoErr::Invalid);
    }
}

bool TlsCertificate::allows_key_usage(TlsCertificateKeyUsage usage) const noexcept {
    FIBER_ASSERT(x509_ != nullptr);
    const std::uint32_t required = usage == TlsCertificateKeyUsage::DigitalSignature ? X509v3_KU_DIGITAL_SIGNATURE
                                                                                     : X509v3_KU_KEY_ENCIPHERMENT;
    const auto allowed = X509_get_key_usage(static_cast<X509 *>(x509_));
    if (allowed == 0) {
        ERR_clear_error();
    }
    return (allowed & required) != 0;
}

common::IoResult<TlsCertificate::Validity> TlsCertificate::validity() const noexcept {
    FIBER_ASSERT(x509_ != nullptr);
    const auto *x509 = static_cast<X509 *>(x509_);
    Validity out;
    std::int64_t secs = 0;
    if (ASN1_TIME_to_posix(X509_get0_notBefore(x509), &secs) != 1) {
        return std::unexpected(common::IoErr::Invalid);
    }
    out.not_before_ms = secs * 1000;
    if (ASN1_TIME_to_posix(X509_get0_notAfter(x509), &secs) != 1) {
        return std::unexpected(common::IoErr::Invalid);
    }
    out.not_after_ms = secs * 1000;
    return out;
}

bool TlsCertificate::matches_host(std::string_view dns_name) const noexcept {
    FIBER_ASSERT(x509_ != nullptr);
    // SAN-only: never fall back to subject CN (BoringSSL's own default list
    // of check flags does not include this, but their docs direct callers to
    // it; the modern TLS behavior is SAN-only).
    const int ok = X509_check_host(static_cast<X509 *>(x509_), dns_name.data(), dns_name.size(),
                                   X509_CHECK_FLAG_NEVER_CHECK_SUBJECT, nullptr);
    return ok == 1;
}

bool TlsCertificate::matches_ip(std::span<const std::uint8_t> ip) const noexcept {
    FIBER_ASSERT(x509_ != nullptr);
    if (ip.size() != 4 && ip.size() != 16) {
        return false;
    }
    return X509_check_ip(static_cast<X509 *>(x509_), ip.data(), ip.size(), 0) == 1;
}

common::IoResult<bool> TlsCertificate::matches_private_key(const TlsPrivateKey &key) const noexcept {
    FIBER_ASSERT(x509_ != nullptr && key.impl_ != nullptr);
    EVP_PKEY *pub = X509_get0_pubkey(static_cast<X509 *>(x509_));
    if (pub == nullptr) {
        ERR_clear_error();
        return std::unexpected(common::IoErr::Invalid);
    }
    return EVP_PKEY_cmp(pub, static_cast<const EVP_PKEY *>(key.impl_)) == 1;
}

common::IoResult<std::size_t> TlsCertificate::subject_line(std::span<char> out) const noexcept {
    FIBER_ASSERT(x509_ != nullptr);
    return name_line(X509_get_subject_name(static_cast<X509 *>(x509_)), out);
}

common::IoResult<std::size_t> TlsCertificate::issuer_line(std::span<char> out) const noexcept {
    FIBER_ASSERT(x509_ != nullptr);
    return name_line(X509_get_issuer_name(static_cast<X509 *>(x509_)), out);
}

// ---- TlsCertificateChain ----

common::IoResult<TlsCertificateChain> TlsCertificateChain::parse_pem_bundle(std::span<const char> pem) noexcept {
    auto ders = decode_pem_certificates(pem, kMaxCerts);
    if (!ders.has_value()) {
        return std::unexpected(ders.error());
    }
    TlsCertificateChain chain;
    for (const std::string &der: *ders) {
        auto cert = TlsCertificate::parse_der({reinterpret_cast<const std::uint8_t *>(der.data()), der.size()});
        if (!cert.has_value()) {
            return std::unexpected(cert.error());
        }
        chain.certs_[chain.count_++] = std::move(*cert);
    }
    return chain;
}

common::IoResult<TlsCertificateChain>
TlsCertificateChain::from_der_list(std::span<const std::span<const std::uint8_t>> ders) noexcept {
    if (ders.empty() || ders.size() > kMaxCerts) {
        return std::unexpected(common::IoErr::Invalid);
    }
    TlsCertificateChain chain;
    for (std::span<const std::uint8_t> der: ders) {
        auto cert = TlsCertificate::parse_der(der);
        if (!cert.has_value()) {
            return std::unexpected(cert.error());
        }
        chain.certs_[chain.count_++] = std::move(*cert);
    }
    return chain;
}

const TlsCertificate &TlsCertificateChain::leaf() const noexcept {
    FIBER_ASSERT(count_ > 0);
    return certs_[0];
}

std::span<const TlsCertificate> TlsCertificateChain::intermediates() const noexcept {
    return {certs_.data() + 1, count_ > 0 ? static_cast<std::size_t>(count_ - 1) : 0};
}

// ---- TlsTrustStore ----

TlsTrustStore::TlsTrustStore(TlsTrustStore &&other) noexcept : store_(other.store_) { other.store_ = nullptr; }

TlsTrustStore &TlsTrustStore::operator=(TlsTrustStore &&other) noexcept {
    if (this != &other) {
        this->~TlsTrustStore();
        store_ = other.store_;
        other.store_ = nullptr;
    }
    return *this;
}

TlsTrustStore::~TlsTrustStore() {
    if (store_ != nullptr) {
        X509_STORE_free(static_cast<X509_STORE *>(store_));
    }
}

namespace {

// Shared store builder: parse each DER and hand it to the store (which takes
// its own reference; the parse temporaries die at scope end).
common::IoErr add_roots(X509_STORE *store, std::span<const std::uint8_t> der) noexcept {
    auto raw = parse_raw(der);
    if (!raw.has_value()) {
        return raw.error();
    }
    if (X509_STORE_add_cert(store, raw->x509) != 1) {
        ERR_clear_error();
        raw_cert_free(*raw);
        return common::IoErr::Invalid;
    }
    raw_cert_free(*raw);
    return common::IoErr::None;
}

} // namespace

common::IoResult<TlsTrustStore>
TlsTrustStore::from_der_roots(std::span<const std::span<const std::uint8_t>> ders) noexcept {
    if (ders.empty()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    X509_STORE *store = X509_STORE_new();
    if (store == nullptr) {
        ERR_clear_error();
        return std::unexpected(common::IoErr::NoMem);
    }
    for (std::span<const std::uint8_t> der: ders) {
        if (const common::IoErr err = add_roots(store, der); err != common::IoErr::None) {
            X509_STORE_free(store);
            return std::unexpected(err);
        }
    }
    TlsTrustStore anchors;
    anchors.store_ = store;
    return anchors;
}

common::IoResult<TlsTrustStore> TlsTrustStore::from_pem_bundle(std::span<const char> pem) noexcept {
    // System root bundles run to hundreds of certificates — no kMaxCerts
    // ceiling here, just a generous sanity bound.
    constexpr std::size_t kMaxRoots = 512;
    auto ders = decode_pem_certificates(pem, kMaxRoots);
    if (!ders.has_value()) {
        return std::unexpected(ders.error());
    }
    X509_STORE *store = X509_STORE_new();
    if (store == nullptr) {
        ERR_clear_error();
        return std::unexpected(common::IoErr::NoMem);
    }
    for (const std::string &der: *ders) {
        if (const common::IoErr err =
                    add_roots(store, {reinterpret_cast<const std::uint8_t *>(der.data()), der.size()});
            err != common::IoErr::None) {
            X509_STORE_free(store);
            return std::unexpected(err);
        }
    }
    TlsTrustStore anchors;
    anchors.store_ = store;
    return anchors;
}

namespace {

// System root bundle candidate, mirroring net's TrustStore candidate walk
// (09 §4.3 — both stacks must resolve the same file). SSL_CERT_FILE wins
// when readable.
const char *system_ca_bundle_candidate() noexcept {
    if (const char *env = std::getenv("SSL_CERT_FILE")) {
        if (env[0] != '\0' && ::access(env, R_OK) == 0) {
            return env;
        }
    }
    static constexpr const char *kCandidates[] = {
            "/etc/ssl/certs/ca-certificates.crt",     "/etc/pki/tls/cert.pem",
            "/etc/ssl/certs/ca-bundle.crt",           "/etc/ssl/cert.pem",
            "/usr/local/share/certs/ca-root-nss.crt", "/etc/openssl/certs/ca-certificates.crt",
    };
    for (const char *candidate: kCandidates) {
        if (::access(candidate, R_OK) == 0) {
            return candidate;
        }
    }
    return nullptr;
}

// One-shot configuration-file read (system bundles run ~200 KiB) into a
// nothrow allocation; the 4 MiB bound is a sanity ceiling, not policy.
std::unique_ptr<char[]> read_system_bundle(const char *path, std::size_t &out_len) noexcept {
    std::FILE *file = std::fopen(path, "rb");
    if (file == nullptr) {
        return nullptr;
    }
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return nullptr;
    }
    const long size = std::ftell(file);
    if (size <= 0 || static_cast<unsigned long>(size) > (1u << 22) || std::fseek(file, 0, SEEK_SET) != 0) {
        std::fclose(file);
        return nullptr;
    }
    std::unique_ptr<char[]> pem(new (std::nothrow) char[static_cast<std::size_t>(size)]);
    if (pem == nullptr ||
        std::fread(pem.get(), 1, static_cast<std::size_t>(size), file) != static_cast<std::size_t>(size)) {
        std::fclose(file);
        return nullptr;
    }
    std::fclose(file);
    out_len = static_cast<std::size_t>(size);
    return pem;
}

} // namespace

TlsTrustStore *TlsTrustStore::system_default() noexcept {
    struct SystemCache {
        TlsTrustStore store;
        bool ok = false;

        SystemCache() noexcept {
            if (const char *path = system_ca_bundle_candidate()) {
                std::size_t len = 0;
                if (std::unique_ptr<char[]> pem = read_system_bundle(path, len)) {
                    auto loaded = TlsTrustStore::from_pem_bundle({pem.get(), len});
                    if (loaded.has_value()) {
                        store = std::move(*loaded);
                        ok = true;
                        return;
                    }
                }
            }
            // No readable bundle (or it failed to parse): OpenSSL's
            // compiled-in default path lookup.
            X509_STORE *fallback = X509_STORE_new();
            if (fallback == nullptr) {
                ERR_clear_error();
                return;
            }
            if (X509_STORE_set_default_paths(fallback) == 1) {
                store.store_ = fallback;
                ok = true;
                return;
            }
            X509_STORE_free(fallback);
            ERR_clear_error();
        }
    };
    // Function-local static init is thread-safe; the holder caches both the
    // resolved store and the miss — neither the filesystem nor the PEM
    // parser runs more than once per process, and the result is never freed.
    static SystemCache *const cached = new (std::nothrow) SystemCache{};
    return cached != nullptr && cached->ok ? &cached->store : nullptr;
}

// ---- tls_verify_chain ----

common::IoResult<TlsCertVerification> tls_verify_chain(const TlsCertificateChain &chain, const TlsTrustStore &anchors,
                                                       TlsCertPurpose purpose, std::string_view host,
                                                       std::span<const std::uint8_t> ip,
                                                       std::int64_t now_unix_ms) noexcept {
    if (chain.empty() || anchors.store_ == nullptr) {
        return std::unexpected(common::IoErr::Invalid);
    }

    // Name check first: a wrong name short-circuits before any chain
    // building (§6.4) and reports the same error code the param-based path
    // would have produced.
    if (!host.empty() && !chain.leaf().matches_host(host)) {
        return TlsCertVerification{TlsCertVerification::Status::NotTrusted, TlsAlertDesc::BadCertificate,
                                   X509_V_ERR_HOSTNAME_MISMATCH};
    }
    if (!ip.empty() && !chain.leaf().matches_ip(ip)) {
        return TlsCertVerification{TlsCertVerification::Status::NotTrusted, TlsAlertDesc::BadCertificate,
                                   X509_V_ERR_IP_ADDRESS_MISMATCH};
    }

    // Untrusted intermediates: borrowed X509 pointers into the chain; the
    // stack is ours, the certificates are not.
    STACK_OF(X509) *untrusted = sk_X509_new_null();
    if (untrusted == nullptr) {
        ERR_clear_error();
        return std::unexpected(common::IoErr::NoMem);
    }
    for (const TlsCertificate &cert: chain.intermediates()) {
        sk_X509_push(untrusted, static_cast<X509 *>(cert.x509_));
    }

    TlsCertVerification result;
    X509_STORE_CTX *ctx = X509_STORE_CTX_new();
    if (ctx == nullptr) {
        ERR_clear_error();
        sk_X509_free(untrusted);
        return std::unexpected(common::IoErr::NoMem);
    }
    // A failed X509_STORE_CTX_init leaves the ctx zeroed by its own cleanup,
    // so the single X509_STORE_CTX_free on the way out stays balanced either
    // way.
    const bool inited = X509_STORE_CTX_init(ctx, static_cast<X509_STORE *>(anchors.store_),
                                            static_cast<X509 *>(chain.leaf().x509_), untrusted) == 1;
    if (inited) {
        // Injected time, plus the purpose and trust BoringSSL's
        // ssl_verify_cert_chain inherits via X509_STORE_CTX_set_default
        // ("ssl_client"/"ssl_server") — purpose is the EKU check, trust adds
        // the anchor-side trust check (a coding-only anchor would be rejected
        // for TLS).
        X509_STORE_CTX_set_time_posix(ctx, 0, now_unix_ms / 1000);
        X509_VERIFY_PARAM *param = X509_STORE_CTX_get0_param(ctx);
        if (purpose == TlsCertPurpose::SslClient) {
            X509_VERIFY_PARAM_set_purpose(param, X509_PURPOSE_SSL_CLIENT);
            X509_VERIFY_PARAM_set_trust(param, X509_TRUST_SSL_CLIENT);
        } else {
            X509_VERIFY_PARAM_set_purpose(param, X509_PURPOSE_SSL_SERVER);
            X509_VERIFY_PARAM_set_trust(param, X509_TRUST_SSL_SERVER);
        }

        if (X509_verify_cert(ctx) == 1) {
            result.status = TlsCertVerification::Status::Trusted;
            result.verify_error = 0;
        } else {
            const int err = X509_STORE_CTX_get_error(ctx);
            result.status = TlsCertVerification::Status::NotTrusted;
            result.verify_error = err;
            result.alert = alert_from_verify_error(err);
        }
    }
    ERR_clear_error();
    X509_STORE_CTX_free(ctx); // does not free the borrowed untrusted stack
    sk_X509_free(untrusted);
    if (!inited) {
        return std::unexpected(common::IoErr::Unknown);
    }
    return result;
}

} // namespace fiber::tls
