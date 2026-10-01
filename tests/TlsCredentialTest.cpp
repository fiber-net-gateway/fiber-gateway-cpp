#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <unistd.h>

#include <fiber/common/IoError.h>
#include <fiber/net/TlsCredential.h>
#include <fiber/net/TrustStore.h>
#include "QuicTestTlsCertificate.h"
#include "TlsClientIdentityTestData.h"

namespace {

// system_ca_bundle_path() must return either an empty string (nothing found)
// or a path that is actually readable on this host.
TEST(TrustStoreSystemCa, ReturnsEmptyOrReadablePath) {
    const std::string &path = fiber::net::TrustStore::system_ca_bundle_path();
    if (path.empty()) {
        SUCCEED() << "no system CA bundle discovered on this host";
        return;
    }
    ASSERT_EQ(::access(path.c_str(), R_OK), 0) << "discovered path not readable: " << path;
}

// Explicit system trust loading must work even when discovery falls back to
// the TLS library's default verify paths.
TEST(TrustStoreSystemCa, CreatesStoreWithSystemTrustAnchors) {
    const auto result = fiber::net::TrustStore::create(fiber::net::TrustStoreOptions::system());
    ASSERT_TRUE(result.has_value()) << "create failed with io_error=" << fiber::common::io_err_name(result.error());
}

TEST(TrustStoreTest, PemContentCreatesStore) {
    const auto store = fiber::net::TrustStore::create(
            fiber::net::TrustStoreOptions::from_content(std::string(fiber::test::kQuicTestCertificatePem)));
    ASSERT_TRUE(store.has_value()) << "create failed with io_error=" << fiber::common::io_err_name(store.error());
}

TEST(TlsCredentialTest, PemContentCreatesCredential) {
    fiber::net::TlsCredentialOptions options{};
    options.certificate_chain =
            fiber::net::TlsPemSource::from_content(std::string(fiber::test::kQuicTestCertificatePem));
    options.private_key = fiber::net::TlsPemSource::from_content(std::string(fiber::test::kQuicTestPrivateKeyPem));
    const auto credential = fiber::net::TlsCredential::create(options);
    ASSERT_TRUE(credential.has_value()) << "create failed with io_error="
                                        << fiber::common::io_err_name(credential.error());
}

// A handle copy shares the material under one reference count; moves and
// resets hand references over or drop them without touching the material.
TEST(TlsCredentialTest, HandlesShareOneReferenceCount) {
    fiber::net::TlsCredentialOptions options{};
    options.certificate_chain =
            fiber::net::TlsPemSource::from_content(std::string(fiber::test::kQuicTestCertificatePem));
    options.private_key = fiber::net::TlsPemSource::from_content(std::string(fiber::test::kQuicTestPrivateKeyPem));
    auto created = fiber::net::TlsCredential::create(options);
    ASSERT_TRUE(created.has_value());

    fiber::net::TlsCredential first = std::move(*created);
    EXPECT_TRUE(created->empty());
    EXPECT_EQ(first.use_count(), 1u);

    fiber::net::TlsCredential second = first;
    EXPECT_EQ(first.use_count(), 2u);
    fiber::net::TlsCredential &alias = second;
    second = alias; // self-assignment keeps the count balanced
    EXPECT_EQ(first.use_count(), 2u);

    fiber::net::TlsCredential third = std::move(second);
    EXPECT_TRUE(second.empty());
    EXPECT_EQ(third.use_count(), 2u);
    third = first; // same material: retain-then-release
    EXPECT_EQ(first.use_count(), 2u);

    third.reset();
    EXPECT_TRUE(third.empty());
    EXPECT_EQ(third.use_count(), 0u);
    EXPECT_EQ(first.use_count(), 1u);

    first = fiber::net::TlsCredential{};
    EXPECT_TRUE(first.empty());
}

// The tls pem setters do not check key/chain pairing, so create() must reject
// a private key that does not match the leaf certificate at creation time.
TEST(TlsCredentialTest, MismatchedKeyPairIsRejected) {
    fiber::net::TlsCredentialOptions options{};
    options.certificate_chain =
            fiber::net::TlsPemSource::from_content(std::string(fiber::test::kQuicTestCertificatePem));
    options.private_key = fiber::net::TlsPemSource::from_content(std::string(fiber::test::kWrongKeyPem));
    const auto credential = fiber::net::TlsCredential::create(options);
    ASSERT_FALSE(credential.has_value());
    EXPECT_EQ(credential.error(), fiber::common::IoErr::Invalid);
}

TEST(TrustStoreSystemCa, SystemDefaultIsCachedSingleton) {
    const auto first = fiber::net::TrustStore::system_default();
    const auto second = fiber::net::TrustStore::system_default();
    if (!first && first.error() == fiber::common::IoErr::NotFound) {
        ASSERT_FALSE(second.has_value());
        EXPECT_EQ(second.error(), fiber::common::IoErr::NotFound);
        GTEST_SKIP() << "no system CA bundle discovered on this host";
    }
    ASSERT_TRUE(first.has_value()) << "system_default failed with io_error="
                                   << fiber::common::io_err_name(first.error());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*first, *second);
}

} // namespace
