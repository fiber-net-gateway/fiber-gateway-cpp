#include <gtest/gtest.h>

#include <openssl/x509.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "TlsCertFixtures.h"

#include <fiber/common/util/Base64.h>
#include <fiber/tls/crypto/TlsCertificate.h>
#include <fiber/tls/crypto/TlsSignature.h>

namespace common = fiber::common;
using namespace fiber::tls;

namespace {

// Every certificate below comes from tests/tls_certs/gen_tls_certs.py
// (tree anchored around certfix::kRefNowMs). Expected X509_V_ERR codes pin
// BoringSSL's path-validation behavior; the alert expectations pin our §4.5
// mapping over it.

std::span<const char> pem_span(const char *pem) { return {pem, std::strlen(pem)}; }

// DER body of the first CERTIFICATE block of a PEM (test-side re-encoding,
// independent of the production loader).
std::string pem_block_der(const char *pem) {
    const std::string_view text(pem);
    constexpr std::string_view kBegin = "-----BEGIN CERTIFICATE-----";
    constexpr std::string_view kEnd = "-----END CERTIFICATE-----";
    const std::size_t b = text.find(kBegin);
    const std::size_t e = text.find(kEnd);
    EXPECT_NE(std::string_view::npos, b);
    EXPECT_NE(std::string_view::npos, e);
    std::string b64;
    for (std::size_t i = b + kBegin.size(); i < e; ++i) {
        const char c = text[i];
        if (c != '\n' && c != '\r') {
            b64.push_back(c);
        }
    }
    std::string der;
    EXPECT_TRUE(fiber::util::base64_decode(b64, der));
    return der;
}

std::span<const std::uint8_t> der_span(const std::string &der) {
    return {reinterpret_cast<const std::uint8_t *>(der.data()), der.size()};
}

std::string join_pems(std::initializer_list<const char *> pems) {
    std::string out;
    for (const char *pem: pems) {
        out += pem;
    }
    return out;
}

// Chain + store + verify in one step for the table-driven cases.
common::IoResult<TlsCertVerification> verify(std::initializer_list<const char *> chain_pems,
                                             std::initializer_list<const char *> root_pems, TlsCertPurpose purpose,
                                             std::string_view host, std::span<const std::uint8_t> ip,
                                             std::int64_t now_ms) {
    const std::string chain_bundle = join_pems(chain_pems);
    auto chain = TlsCertificateChain::parse_pem_bundle({chain_bundle.data(), chain_bundle.size()});
    EXPECT_TRUE(chain.has_value());
    const std::string root_bundle = join_pems(root_pems);
    auto store = TlsTrustStore::from_pem_bundle({root_bundle.data(), root_bundle.size()});
    EXPECT_TRUE(store.has_value());
    if (!chain.has_value() || !store.has_value()) {
        return std::unexpected(common::IoErr::Unknown);
    }
    return tls_verify_chain(*chain, *store, purpose, host, ip, now_ms);
}

void expect_not_trusted(const common::IoResult<TlsCertVerification> &res, int want_err, TlsAlertDesc want_alert) {
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(TlsCertVerification::Status::NotTrusted, res->status);
    EXPECT_EQ(want_err, res->verify_error);
    EXPECT_EQ(want_alert, res->alert);
}

constexpr std::array<std::uint8_t, 4> kLeafIp{192, 168, 7, 1};

} // namespace

// ---------------------------------------------------------------------------
// Certificate parsing
// ---------------------------------------------------------------------------

TEST(TlsCertificateParse, DerRoundTrip) {
    const std::string der = pem_block_der(certfix::kLeafRsaPem);
    auto cert = TlsCertificate::parse_der(der_span(der));
    ASSERT_TRUE(cert.has_value());
    // der() is a byte-exact view of the parsed DER.
    ASSERT_EQ(der.size(), cert->der().size());
    EXPECT_EQ(0, std::memcmp(der.data(), cert->der().data(), der.size()));

    EXPECT_FALSE(cert->public_key()->empty());
    EXPECT_EQ(TlsKeyKind::Rsa, cert->public_key()->key_kind());

    // Move keeps the view intact.
    TlsCertificate moved = std::move(*cert);
    ASSERT_EQ(der.size(), moved.der().size());
    EXPECT_EQ(0, std::memcmp(der.data(), moved.der().data(), der.size()));
}

TEST(TlsCertificateParse, RejectsBadInput) {
    EXPECT_EQ(TlsCertificate::parse_der({}).error(), common::IoErr::Invalid);
    const std::uint8_t garbage[] = {0x30, 0x03, 0x02, 0x01};
    EXPECT_EQ(TlsCertificate::parse_der(garbage).error(), common::IoErr::Invalid);
    // Over the 1 MiB sanity bound.
    const std::string huge(TlsCertificate::kMaxDerLen + 1, '\0');
    EXPECT_EQ(TlsCertificate::parse_der(der_span(huge)).error(), common::IoErr::Invalid);
}

// ---------------------------------------------------------------------------
// Chain loading
// ---------------------------------------------------------------------------

TEST(TlsCertificateChainParse, PemBundle) {
    const std::string three = join_pems({certfix::kLeafRsaPem, certfix::kIntermediateRsaPem, certfix::kRootRsaPem});
    auto chain = TlsCertificateChain::parse_pem_bundle({three.data(), three.size()});
    ASSERT_TRUE(chain.has_value());
    EXPECT_EQ(3u, chain->size());
    EXPECT_FALSE(chain->empty());

    const std::string leaf_der = pem_block_der(certfix::kLeafRsaPem);
    ASSERT_EQ(leaf_der.size(), chain->leaf().der().size());
    EXPECT_EQ(0, std::memcmp(leaf_der.data(), chain->leaf().der().data(), leaf_der.size()));
    EXPECT_EQ(2u, chain->intermediates().size());

    // Full capacity is fine.
    const std::string four = join_pems(
            {certfix::kLeafRsaPem, certfix::kIntermediateRsaPem, certfix::kRootRsaPem, certfix::kRootUnrelatedPem});
    auto full = TlsCertificateChain::parse_pem_bundle({four.data(), four.size()});
    ASSERT_TRUE(full.has_value());
    EXPECT_EQ(4u, full->size());

    // One over capacity is a configuration error.
    const std::string five = four + certfix::kRootUnrelatedPem;
    auto over = TlsCertificateChain::parse_pem_bundle({five.data(), five.size()});
    ASSERT_FALSE(over.has_value());
    EXPECT_EQ(common::IoErr::MessageTooLarge, over.error());

    // No CERTIFICATE block at all.
    auto none = TlsCertificateChain::parse_pem_bundle(pem_span("hello"));
    ASSERT_FALSE(none.has_value());
    EXPECT_EQ(common::IoErr::Invalid, none.error());
}

TEST(TlsCertificateChainParse, FromDerList) {
    const std::string leaf = pem_block_der(certfix::kLeafRsaPem);
    const std::string intermediate = pem_block_der(certfix::kIntermediateRsaPem);
    const std::string root = pem_block_der(certfix::kRootRsaPem);
    const std::span<const std::uint8_t> ders[] = {der_span(leaf), der_span(intermediate), der_span(root)};
    auto chain = TlsCertificateChain::from_der_list(ders);
    ASSERT_TRUE(chain.has_value());
    EXPECT_EQ(3u, chain->size());

    EXPECT_EQ(common::IoErr::Invalid, TlsCertificateChain::from_der_list({}).error());
}

TEST(TlsCertificateChainDeath, LeafOnEmptyChain) {
    const TlsCertificateChain chain;
    EXPECT_DEATH((void) chain.leaf(), "FIBER_ASSERT failed");
}

// ---------------------------------------------------------------------------
// Validity / name matching / key pairing / name lines
// ---------------------------------------------------------------------------

TEST(TlsCertificateValidity, FixtureAnchors) {
    // Generation truncates the anchor to whole milliseconds while the ASN.1
    // times keep full precision — compare within one second.
    auto leaf = TlsCertificate::parse_der(der_span(pem_block_der(certfix::kLeafRsaPem)));
    ASSERT_TRUE(leaf.has_value());
    auto v = leaf->validity();
    ASSERT_TRUE(v.has_value());
    EXPECT_LE(v->not_before_ms - certfix::kRefNowMs, certfix::kDayMs / 24);
    EXPECT_GE(certfix::kRefNowMs - v->not_before_ms, -certfix::kDayMs / 24);
    EXPECT_GT(v->not_after_ms, certfix::kRefNowMs);
    EXPECT_LT(v->not_before_ms, v->not_after_ms);

    auto expired = TlsCertificate::parse_der(der_span(pem_block_der(certfix::kLeafExpiredPem)));
    ASSERT_TRUE(expired.has_value());
    v = expired->validity();
    ASSERT_TRUE(v.has_value());
    EXPECT_LT(v->not_after_ms, certfix::kRefNowMs - 4 * certfix::kDayMs);

    auto future = TlsCertificate::parse_der(der_span(pem_block_der(certfix::kLeafFuturePem)));
    ASSERT_TRUE(future.has_value());
    v = future->validity();
    ASSERT_TRUE(v.has_value());
    EXPECT_GT(v->not_before_ms, certfix::kRefNowMs + 364 * certfix::kDayMs);
}

TEST(TlsCertificateMatch, HostWildcardAndCase) {
    auto leaf = TlsCertificate::parse_der(der_span(pem_block_der(certfix::kLeafRsaPem)));
    ASSERT_TRUE(leaf.has_value()); // SANs: example.com, *.example.com, 192.168.7.1

    EXPECT_TRUE(leaf->matches_host("example.com"));
    EXPECT_TRUE(leaf->matches_host("EXAMPLE.com")); // case-insensitive
    EXPECT_TRUE(leaf->matches_host("www.example.com")); // single-label wildcard
    EXPECT_TRUE(leaf->matches_host("a.example.com"));
    EXPECT_FALSE(leaf->matches_host("a.b.example.com")); // wildcard covers one label only
    EXPECT_FALSE(leaf->matches_host("example.org"));
    EXPECT_FALSE(leaf->matches_host("com"));
    EXPECT_FALSE(leaf->matches_host("example.com.")); // no trailing-dot stripping

    // SAN-only: CN is never consulted.
    auto cnonly = TlsCertificate::parse_der(der_span(pem_block_der(certfix::kLeafCnOnlyPem)));
    ASSERT_TRUE(cnonly.has_value()); // CN=example.com, no SAN
    EXPECT_FALSE(cnonly->matches_host("example.com"));
}

TEST(TlsCertificateMatch, Ip) {
    auto leaf = TlsCertificate::parse_der(der_span(pem_block_der(certfix::kLeafRsaPem)));
    ASSERT_TRUE(leaf.has_value());

    EXPECT_TRUE(leaf->matches_ip(kLeafIp));
    const std::array<std::uint8_t, 4> other{192, 168, 7, 2};
    EXPECT_FALSE(leaf->matches_ip(other));
    const std::array<std::uint8_t, 16> v6{};
    EXPECT_FALSE(leaf->matches_ip(v6));
    const std::uint8_t bad_size[] = {1, 2, 3, 4, 5};
    EXPECT_FALSE(leaf->matches_ip(bad_size));
}

TEST(TlsCertificateMatch, PrivateKeyPairing) {
    struct Case {
        const char *cert_pem;
        const char *key_pem;
        bool matches;
    };
    const Case cases[] = {
            {certfix::kLeafRsaPem, certfix::kRsa2048KeyPem, true},
            {certfix::kLeafRsaPem, certfix::kRsa1024KeyPem, false},
            {certfix::kLeafEcP256Pem, certfix::kP256KeyPem, true},
            {certfix::kLeafEcP256Pem, certfix::kP384KeyPem, false},
            {certfix::kLeafEcP384Pem, certfix::kP384KeyPem, true},
            {certfix::kCertP521Pem, certfix::kP521KeyPem, true},
            {certfix::kLeafEd25519Pem, certfix::kEd25519KeyPem, true},
            {certfix::kLeafEd25519Pem, certfix::kRsa2048KeyPem, false},
    };
    for (const Case &c: cases) {
        auto cert = TlsCertificate::parse_der(der_span(pem_block_der(c.cert_pem)));
        ASSERT_TRUE(cert.has_value());
        auto key = TlsPrivateKey::parse_pem(pem_span(c.key_pem));
        ASSERT_TRUE(key.has_value());
        auto res = cert->matches_private_key(*key);
        ASSERT_TRUE(res.has_value());
        EXPECT_EQ(c.matches, *res);
    }
}

TEST(TlsCertificateNames, SubjectIssuerLines) {
    auto leaf = TlsCertificate::parse_der(der_span(pem_block_der(certfix::kLeafRsaPem)));
    ASSERT_TRUE(leaf.has_value());

    char buf[256];
    auto subj = leaf->subject_line(buf);
    ASSERT_TRUE(subj.has_value());
    EXPECT_NE(std::string_view::npos, std::string_view(buf, *subj).find("leaf.rsa.example.com"));

    auto issuer = leaf->issuer_line(buf);
    ASSERT_TRUE(issuer.has_value());
    EXPECT_NE(std::string_view::npos, std::string_view(buf, *issuer).find("Test Intermediate CA"));

    // BoringSSL's X509_NAME_oneline truncates by omission when an entry does
    // not fit (it stops at the last complete entry rather than failing), so a
    // buffer too small for the first entry yields an empty line — callers
    // pass log-line-sized buffers.
    char tiny[4];
    auto small = leaf->subject_line(tiny);
    ASSERT_TRUE(small.has_value());
    EXPECT_EQ(0u, *small);
    EXPECT_EQ('\0', tiny[0]);
}

// ---------------------------------------------------------------------------
// Trust store loading
// ---------------------------------------------------------------------------

TEST(TlsTrustStoreParse, Builders) {
    const std::string bundle = join_pems({certfix::kRootRsaPem, certfix::kRootUnrelatedPem});
    auto store = TlsTrustStore::from_pem_bundle({bundle.data(), bundle.size()});
    EXPECT_TRUE(store.has_value());

    const std::string der = pem_block_der(certfix::kRootRsaPem);
    const std::span<const std::uint8_t> ders[] = {der_span(der)};
    auto from_der = TlsTrustStore::from_der_roots(ders);
    EXPECT_TRUE(from_der.has_value());

    EXPECT_FALSE(TlsTrustStore::from_pem_bundle(pem_span("garbage")).has_value());
    EXPECT_FALSE(TlsTrustStore::from_der_roots({}).has_value());
}

// ---------------------------------------------------------------------------
// Path validation
// ---------------------------------------------------------------------------

TEST(TlsVerifyChain, TrustedServerChain) {
    // Leaf + intermediate, root as the only anchor.
    auto res = verify({certfix::kLeafRsaPem, certfix::kIntermediateRsaPem}, {certfix::kRootRsaPem},
                      TlsCertPurpose::SslServer, "example.com", {}, certfix::kRefNowMs);
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(TlsCertVerification::Status::Trusted, res->status);
    EXPECT_EQ(0, res->verify_error);

    // Sending the root inside the chain as well is tolerated.
    auto with_root = verify({certfix::kLeafRsaPem, certfix::kIntermediateRsaPem, certfix::kRootRsaPem},
                            {certfix::kRootRsaPem}, TlsCertPurpose::SslServer, "example.com", {}, certfix::kRefNowMs);
    ASSERT_TRUE(with_root.has_value());
    EXPECT_EQ(TlsCertVerification::Status::Trusted, with_root->status);

    // Direct root issue (no intermediate), and IP-form reference identity.
    auto direct = verify({certfix::kCertP521Pem}, {certfix::kRootRsaPem}, TlsCertPurpose::SslServer, "example.com", {},
                         certfix::kRefNowMs);
    ASSERT_TRUE(direct.has_value());
    EXPECT_EQ(TlsCertVerification::Status::Trusted, direct->status);

    auto by_ip = verify({certfix::kLeafRsaPem, certfix::kIntermediateRsaPem}, {certfix::kRootRsaPem},
                        TlsCertPurpose::SslServer, "", kLeafIp, certfix::kRefNowMs);
    ASSERT_TRUE(by_ip.has_value());
    EXPECT_EQ(TlsCertVerification::Status::Trusted, by_ip->status);
}

TEST(TlsVerifyChain, TrustedClientChainMtls) {
    // Server verifying a client chain (mTLS): empty host/ip must not skip
    // validation — the client_rsa chain is still fully checked.
    auto res = verify({certfix::kClientRsaPem, certfix::kIntermediateRsaPem}, {certfix::kRootRsaPem},
                      TlsCertPurpose::SslClient, "", {}, certfix::kRefNowMs);
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(TlsCertVerification::Status::Trusted, res->status);
    EXPECT_EQ(0, res->verify_error);
}

TEST(TlsVerifyChain, PurposeMismatch) {
    // serverAuth-only leaf presented as a client chain.
    auto as_client = verify({certfix::kLeafRsaPem, certfix::kIntermediateRsaPem}, {certfix::kRootRsaPem},
                            TlsCertPurpose::SslClient, "", {}, certfix::kRefNowMs);
    expect_not_trusted(as_client, X509_V_ERR_INVALID_PURPOSE, TlsAlertDesc::UnsupportedCertificate);

    // clientAuth-only leaf presented as a server chain.
    auto as_server = verify({certfix::kClientRsaPem, certfix::kIntermediateRsaPem}, {certfix::kRootRsaPem},
                            TlsCertPurpose::SslServer, "", {}, certfix::kRefNowMs);
    expect_not_trusted(as_server, X509_V_ERR_INVALID_PURPOSE, TlsAlertDesc::UnsupportedCertificate);
}

TEST(TlsVerifyChain, MissingIntermediate) {
    auto res = verify({certfix::kLeafRsaPem}, {certfix::kRootRsaPem}, TlsCertPurpose::SslServer, "example.com", {},
                      certfix::kRefNowMs);
    expect_not_trusted(res, X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY, TlsAlertDesc::UnknownCa);
}

TEST(TlsVerifyChain, UnrelatedAnchor) {
    auto res = verify({certfix::kLeafUnrelatedPem}, {certfix::kRootRsaPem}, TlsCertPurpose::SslServer, "example.com",
                      {}, certfix::kRefNowMs);
    expect_not_trusted(res, X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY, TlsAlertDesc::UnknownCa);
}

TEST(TlsVerifyChain, SelfSignedNotAnchored) {
    auto res = verify({certfix::kLeafSelfSignedPem}, {certfix::kRootRsaPem}, TlsCertPurpose::SslServer, "example.com",
                      {}, certfix::kRefNowMs);
    expect_not_trusted(res, X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT, TlsAlertDesc::UnknownCa);
}

TEST(TlsVerifyChain, ExpiredAndFuture) {
    auto expired = verify({certfix::kLeafExpiredPem, certfix::kIntermediateRsaPem}, {certfix::kRootRsaPem},
                          TlsCertPurpose::SslServer, "example.com", {}, certfix::kRefNowMs);
    expect_not_trusted(expired, X509_V_ERR_CERT_HAS_EXPIRED, TlsAlertDesc::CertificateExpired);

    auto future = verify({certfix::kLeafFuturePem, certfix::kIntermediateRsaPem}, {certfix::kRootRsaPem},
                         TlsCertPurpose::SslServer, "example.com", {}, certfix::kRefNowMs);
    expect_not_trusted(future, X509_V_ERR_CERT_NOT_YET_VALID, TlsAlertDesc::CertificateExpired);
}

TEST(TlsVerifyChain, ValidityBoundaryAtSecondGranularity) {
    // Verification time is whole seconds; check the flip around the root's
    // notAfter using the root itself as leaf (its window outlives everything
    // else, so the boundary is isolated).
    const std::string root_bundle = certfix::kRootRsaPem;
    auto chain = TlsCertificateChain::parse_pem_bundle({root_bundle.data(), root_bundle.size()});
    ASSERT_TRUE(chain.has_value());
    auto store = TlsTrustStore::from_pem_bundle(pem_span(certfix::kRootRsaPem));
    ASSERT_TRUE(store.has_value());
    auto v = chain->leaf().validity();
    ASSERT_TRUE(v.has_value());

    auto before = tls_verify_chain(*chain, *store, TlsCertPurpose::SslServer, "", {}, v->not_after_ms - 1000);
    ASSERT_TRUE(before.has_value());
    EXPECT_EQ(TlsCertVerification::Status::Trusted, before->status);

    auto after = tls_verify_chain(*chain, *store, TlsCertPurpose::SslServer, "", {}, v->not_after_ms + 1000);
    expect_not_trusted(after, X509_V_ERR_CERT_HAS_EXPIRED, TlsAlertDesc::CertificateExpired);
}

TEST(TlsVerifyChain, HostnameMismatch) {
    auto res = verify({certfix::kLeafWrongNamePem, certfix::kIntermediateRsaPem}, {certfix::kRootRsaPem},
                      TlsCertPurpose::SslServer, "example.com", {}, certfix::kRefNowMs);
    expect_not_trusted(res, X509_V_ERR_HOSTNAME_MISMATCH, TlsAlertDesc::BadCertificate);

    // SAN-less leaf (CN=example.com): no CN fallback at verification either.
    auto cnonly = verify({certfix::kLeafCnOnlyPem, certfix::kIntermediateRsaPem}, {certfix::kRootRsaPem},
                         TlsCertPurpose::SslServer, "example.com", {}, certfix::kRefNowMs);
    expect_not_trusted(cnonly, X509_V_ERR_HOSTNAME_MISMATCH, TlsAlertDesc::BadCertificate);
}

TEST(TlsVerifyChain, IpMismatch) {
    const std::array<std::uint8_t, 4> wrong{10, 1, 2, 3};
    auto res = verify({certfix::kLeafRsaPem, certfix::kIntermediateRsaPem}, {certfix::kRootRsaPem},
                      TlsCertPurpose::SslServer, "", wrong, certfix::kRefNowMs);
    expect_not_trusted(res, X509_V_ERR_IP_ADDRESS_MISMATCH, TlsAlertDesc::BadCertificate);
}

TEST(TlsVerifyChain, InvalidArguments) {
    const std::string bundle = certfix::kLeafRsaPem;
    auto chain = TlsCertificateChain::parse_pem_bundle({bundle.data(), bundle.size()});
    ASSERT_TRUE(chain.has_value());
    auto store = TlsTrustStore::from_pem_bundle(pem_span(certfix::kRootRsaPem));
    ASSERT_TRUE(store.has_value());

    const TlsCertificateChain empty_chain;
    EXPECT_EQ(common::IoErr::Invalid,
              tls_verify_chain(empty_chain, *store, TlsCertPurpose::SslServer, "", {}, certfix::kRefNowMs).error());

    const TlsTrustStore empty_store;
    EXPECT_EQ(common::IoErr::Invalid,
              tls_verify_chain(*chain, empty_store, TlsCertPurpose::SslServer, "", {}, certfix::kRefNowMs).error());
}

TEST(TlsVerifyChain, SharedStoreAcrossThreads) {
    // The configuration-built store is shared immutably across workers; this
    // is the concurrency smoke for that sharing.
    const std::string chain_bundle = join_pems({certfix::kLeafRsaPem, certfix::kIntermediateRsaPem});
    auto chain = TlsCertificateChain::parse_pem_bundle({chain_bundle.data(), chain_bundle.size()});
    ASSERT_TRUE(chain.has_value());
    auto store = TlsTrustStore::from_pem_bundle(pem_span(certfix::kRootRsaPem));
    ASSERT_TRUE(store.has_value());

    std::atomic<int> failures{0};
    auto worker = [&]() {
        for (int i = 0; i < 64; ++i) {
            auto res =
                    tls_verify_chain(*chain, *store, TlsCertPurpose::SslServer, "example.com", {}, certfix::kRefNowMs);
            if (!res.has_value() || res->status != TlsCertVerification::Status::Trusted) {
                ++failures;
            }
        }
    };
    std::thread t1(worker), t2(worker);
    t1.join();
    t2.join();
    EXPECT_EQ(0, failures.load());
}

// The process-wide system store (09 §4.3): resolves on this machine, and
// both calls hand back the SAME cached object (success is cached forever —
// the filesystem is not walked twice).
TEST(TlsTrustStoreSystem, SystemDefaultIsCachedPerProcess) {
    TlsTrustStore *first = TlsTrustStore::system_default();
    ASSERT_NE(nullptr, first);
    EXPECT_NE(nullptr, first->x509_store_handle());
    EXPECT_EQ(first, TlsTrustStore::system_default());
    EXPECT_EQ(first->x509_store_handle(), TlsTrustStore::system_default()->x509_store_handle());
}
