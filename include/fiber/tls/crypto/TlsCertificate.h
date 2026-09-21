#ifndef FIBER_TLS_CRYPTO_TLS_CERTIFICATE_H
#define FIBER_TLS_CRYPTO_TLS_CERTIFICATE_H

// Certificate material for the TLS stack (feature/tls/02b §4): single
// certificates (DER-in / DER-out with the parsed X509 behind a pimpl), leaf
// chains, trust anchors, and the X509 path validation entry point. Structure
// checks, name constraints, EKU/purpose and validity all run inside
// X509_verify_cert — this layer only loads material, keeps the DER bytes for
// wire re-serialization, maps results to TLS alerts (§4.5, mirroring
// BoringSSL's SSL_alert_from_verify_result), and injects time and purpose as
// caller parameters (no clock inside the library). No OpenSSL type appears
// here; the handles are void* pimpls freed on the .cpp side.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "../../common/IoError.h"
#include "../TlsTypes.h"
#include "TlsSignature.h"

namespace fiber::tls {

// Which side of the handshake the verified chain belongs to — selects the
// X509 purpose (EKU check) inside path validation, mirroring BoringSSL's
// set_default("ssl_client"|"ssl_server").
enum class TlsCertPurpose : std::uint8_t {
    SslServer, // we are the client, verifying the server's chain
    SslClient, // we are the server, verifying a client chain (mTLS)
};

class TlsCertificateChain;
class TlsTrustStore;
struct TlsCertVerification;
[[nodiscard]] common::IoResult<TlsCertVerification> tls_verify_chain(const TlsCertificateChain &, const TlsTrustStore &,
                                                                     TlsCertPurpose, std::string_view,
                                                                     std::span<const std::uint8_t>,
                                                                     std::int64_t) noexcept;

// A single parsed certificate. Owns both the X509 and the CRYPTO_BUFFER
// holding the original DER (X509_parse_from_buffer takes its own buffer
// reference; the two are released together), so der() is a zero-copy view of
// the exact bytes that were parsed — what the wire re-sends. Move-only.
class TlsCertificate {
public:
    static constexpr std::size_t kMaxDerLen = 1u << 20; // 1 MiB sanity bound

    TlsCertificate() noexcept = default;
    TlsCertificate(const TlsCertificate &) = delete;
    TlsCertificate &operator=(const TlsCertificate &) = delete;
    TlsCertificate(TlsCertificate &&other) noexcept;
    TlsCertificate &operator=(TlsCertificate &&other) noexcept;
    ~TlsCertificate();

    // Shared by wire parsing (06 receives certificate entries) and
    // configuration loading. Invalid = unparsable DER / unsupported key type.
    [[nodiscard]] static common::IoResult<TlsCertificate> parse_der(std::span<const std::uint8_t> der) noexcept;

    // The exact DER bytes handed to parse_der (zero-copy view, lives as long
    // as this object).
    [[nodiscard]] std::span<const std::uint8_t> der() const noexcept;

    // Public key as a borrowed view — its lifetime is this certificate.
    [[nodiscard]] common::IoResult<TlsPublicKeyView> public_key() const noexcept;

    struct Validity {
        std::int64_t not_before_ms = 0;
        std::int64_t not_after_ms = 0;
    };
    // For display / startup self-checks; the authoritative validity decision
    // is X509_verify_cert's with the injected now.
    [[nodiscard]] common::IoResult<Validity> validity() const noexcept;

    // RFC 6125 name matching, SAN-only (X509_CHECK_FLAG_NEVER_CHECK_SUBJECT —
    // no CN fallback), case-insensitive with wildcard SANs. False = mismatch
    // or internal failure; matching is a query, not an error path.
    [[nodiscard]] bool matches_host(std::string_view dns_name) const noexcept;
    // 4-byte IPv4 / 16-byte IPv6 SAN iPAddress match.
    [[nodiscard]] bool matches_ip(std::span<const std::uint8_t> ip) const noexcept;
    // True iff |key| is the private half of this certificate's public key.
    [[nodiscard]] common::IoResult<bool> matches_private_key(const TlsPrivateKey &key) const noexcept;

    // One-line subject/issuer digests (X509_NAME_oneline) for logs and test
    // names; fills |out| (NUL-terminated) and returns the text length.
    [[nodiscard]] common::IoResult<std::size_t> subject_line(std::span<char> out) const noexcept;
    [[nodiscard]] common::IoResult<std::size_t> issuer_line(std::span<char> out) const noexcept;

private:
    friend common::IoResult<TlsCertVerification> tls_verify_chain(const TlsCertificateChain &, const TlsTrustStore &,
                                                                  TlsCertPurpose, std::string_view,
                                                                  std::span<const std::uint8_t>, std::int64_t) noexcept;

    void *x509_ = nullptr; // X509*, owning reference
    void *der_buf_ = nullptr; // CRYPTO_BUFFER*, owning reference (x509_ holds another)
    std::uint32_t der_len_ = 0;
};

// A leaf-first certificate chain (leaf at [0]). Loading only — no structural
// chain validation here (issuer/subject linking is tls_verify_chain's job).
// Capacity 4 = leaf + up to three intermediates, the realistic serving
// configuration; a longer bundle is a configuration error. Move-only.
class TlsCertificateChain {
public:
    static constexpr std::size_t kMaxCerts = 4;

    TlsCertificateChain() noexcept = default;
    TlsCertificateChain(const TlsCertificateChain &) = delete;
    TlsCertificateChain &operator=(const TlsCertificateChain &) = delete;
    TlsCertificateChain(TlsCertificateChain &&) noexcept = default;
    TlsCertificateChain &operator=(TlsCertificateChain &&) noexcept = default;

    // PEM bundle of "CERTIFICATE" blocks, armor stripped without BIO.
    [[nodiscard]] static common::IoResult<TlsCertificateChain> parse_pem_bundle(std::span<const char> pem) noexcept;
    // DER entries whose lengths the 04 codec already decoded (wire path), or
    // a caller-assembled configuration.
    [[nodiscard]] static common::IoResult<TlsCertificateChain>
    from_der_list(std::span<const std::span<const std::uint8_t>> ders) noexcept;

    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
    [[nodiscard]] std::size_t size() const noexcept { return count_; }
    [[nodiscard]] const TlsCertificate &leaf() const noexcept; // asserts non-empty
    [[nodiscard]] std::span<const TlsCertificate> intermediates() const noexcept;

private:
    std::array<TlsCertificate, kMaxCerts> certs_{};
    std::uint8_t count_ = 0;
};

// Trust anchors (X509_STORE* pimpl). Build once at configuration time, share
// immutably across workers afterwards (X509_STORE is concurrency-safe for
// verification when not mutated). Move-only.
class TlsTrustStore {
public:
    TlsTrustStore() noexcept = default;
    TlsTrustStore(const TlsTrustStore &) = delete;
    TlsTrustStore &operator=(const TlsTrustStore &) = delete;
    TlsTrustStore(TlsTrustStore &&other) noexcept;
    TlsTrustStore &operator=(TlsTrustStore &&other) noexcept;
    ~TlsTrustStore();

    [[nodiscard]] static common::IoResult<TlsTrustStore> from_pem_bundle(std::span<const char> pem) noexcept;
    [[nodiscard]] static common::IoResult<TlsTrustStore>
    from_der_roots(std::span<const std::span<const std::uint8_t>> ders) noexcept;

private:
    friend common::IoResult<TlsCertVerification> tls_verify_chain(const TlsCertificateChain &, const TlsTrustStore &,
                                                                  TlsCertPurpose, std::string_view,
                                                                  std::span<const std::uint8_t>, std::int64_t) noexcept;

    void *store_ = nullptr; // X509_STORE*
};

struct TlsCertVerification {
    enum class Status : std::uint8_t { Trusted, NotTrusted };
    Status status = Status::NotTrusted;
    // The alert the engine sends on NotTrusted (§4.5 mapping over
    // BoringSSL's SSL_alert_from_verify_result).
    TlsAlertDesc alert = TlsAlertDesc::BadCertificate;
    // Raw X509_V_ERR_* code (0 = X509_V_OK) for logs and test assertions.
    int verify_error = 0;
};

// Path validation (§4.4): hostname/IP checked against the leaf first
// (short-circuit — a wrong name never triggers chain building), then
// X509_verify_cert with purpose, and |now_unix_ms| as the verification time
// (injected by the caller; seconds precision internally). host/ip may both
// be empty (server verifying a client chain); the chain is still fully
// validated in that case — an empty name does NOT skip verification.
[[nodiscard]] common::IoResult<TlsCertVerification>
tls_verify_chain(const TlsCertificateChain &chain, const TlsTrustStore &anchors, TlsCertPurpose purpose,
                 std::string_view host, std::span<const std::uint8_t> ip, std::int64_t now_unix_ms) noexcept;

} // namespace fiber::tls

#endif // FIBER_TLS_CRYPTO_TLS_CERTIFICATE_H
