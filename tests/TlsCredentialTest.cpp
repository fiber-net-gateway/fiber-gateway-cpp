#include <gtest/gtest.h>

#include <string>
#include <unistd.h>

#include <openssl/ssl.h>

#include <fiber/common/IoError.h>
#include <fiber/net/TlsCredential.h>
#include <fiber/net/TlsServerHandshakeConfig.h>
#include <fiber/net/TrustStore.h>
#include "QuicTestTlsCertificate.h"
#include "net/detail/TlsSslFactory.h"

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

TEST(TlsCredentialTest, SslFactoryBuildsClientAndServerRoles) {
    fiber::net::TlsCredentialOptions credential_options{};
    credential_options.certificate_chain =
            fiber::net::TlsPemSource::from_content(std::string(fiber::test::kQuicTestCertificatePem));
    credential_options.private_key =
            fiber::net::TlsPemSource::from_content(std::string(fiber::test::kQuicTestPrivateKeyPem));
    auto credential = fiber::net::TlsCredential::create(credential_options);
    ASSERT_TRUE(credential.has_value());
    auto trust_store = fiber::net::TrustStore::create(
            fiber::net::TrustStoreOptions::from_content(std::string(fiber::test::kQuicTestCertificatePem)));
    ASSERT_TRUE(trust_store.has_value());

    fiber::net::TlsClientParam client_param{};
    client_param.enable_tls = true;
    client_param.credential = credential->get();
    client_param.trust_store = trust_store->get();
    client_param.verify_peer = true;
    client_param.sni_name = "localhost";
    auto client_ssl = fiber::net::detail::TlsSslFactory::create_client(client_param, nullptr);
    ASSERT_TRUE(client_ssl.has_value()) << "client factory failed with io_error="
                                        << fiber::common::io_err_name(client_ssl.error());
    SSL_free(*client_ssl);

    fiber::net::TlsServerParam server_param{};
    server_param.configure_callback = &fiber::net::configure_tls_with_credential;
    server_param.configure_ctx = credential->get();
    server_param.trust_store = trust_store->get();
    fiber::net::detail::TlsServerHandshakeState state{};
    auto server_ssl = fiber::net::detail::TlsSslFactory::create_server(server_param, state, nullptr, nullptr,
                                                                       fiber::net::TlsTransportKind::Tcp);
    ASSERT_TRUE(server_ssl.has_value()) << "server factory failed with io_error="
                                        << fiber::common::io_err_name(server_ssl.error());
    SSL_free(*server_ssl);
}

TEST(TlsSslFactoryTest, DisabledParamsAreRejected) {
    fiber::net::TlsClientParam client_param{};
    EXPECT_EQ(fiber::net::detail::TlsSslFactory::create_client(client_param, nullptr).error(),
              fiber::common::IoErr::Invalid);

    fiber::net::TlsServerParam server_param{};
    fiber::net::detail::TlsServerHandshakeState state{};
    EXPECT_EQ(fiber::net::detail::TlsSslFactory::create_server(server_param, state, nullptr, nullptr,
                                                               fiber::net::TlsTransportKind::Tcp)
                      .error(),
              fiber::common::IoErr::Invalid);
}

TEST(TlsSslFactoryTest, VerifyPeerWithoutTrustStoreIsRejected) {
    fiber::net::TlsClientParam client_param{};
    client_param.enable_tls = true;
    client_param.verify_peer = true;
    EXPECT_EQ(fiber::net::detail::TlsSslFactory::create_client(client_param, nullptr).error(),
              fiber::common::IoErr::Invalid);
}

} // namespace
