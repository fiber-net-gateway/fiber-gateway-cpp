#include <gtest/gtest.h>

#include <string>
#include <unistd.h>

#include <openssl/ssl.h>

#include <fiber/common/IoError.h>
#include <fiber/net/TlsContext.h>
#include <fiber/net/TlsOptions.h>
#include "QuicTestTlsCertificate.h"

namespace {

// system_ca_bundle_path() must return either an empty string (nothing found)
// or a path that is actually readable on this host.
TEST(TlsContextSystemCa, ReturnsEmptyOrReadablePath) {
    const std::string &path = fiber::net::TlsContext::system_ca_bundle_path();
    if (path.empty()) {
        SUCCEED() << "no system CA bundle discovered on this host";
        return;
    }
    ASSERT_EQ(::access(path.c_str(), R_OK), 0) << "discovered path not readable: " << path;
}

// Explicit system trust loading must work even when discovery falls back to
// the TLS library's default verify paths.
TEST(TlsContextSystemCa, CreatesContextWithSystemTrustStore) {
    fiber::net::TlsOptions options{};
    options.trust_store = fiber::net::TlsTrustStoreSource::system();
    const auto result = fiber::net::TlsContext::create(options);
    ASSERT_TRUE(result.has_value()) << "init failed with io_error=" << fiber::common::io_err_name(result.error());
    EXPECT_TRUE((*result)->has_trust_store());
}

TEST(TlsContextTest, PemContentCreatesRoleNeutralContext) {
    fiber::net::TlsOptions material{};
    material.certificate_chain =
            fiber::net::TlsPemSource::from_content(std::string(fiber::test::kQuicTestCertificatePem));
    material.private_key = fiber::net::TlsPemSource::from_content(std::string(fiber::test::kQuicTestPrivateKeyPem));
    material.trust_store =
            fiber::net::TlsTrustStoreSource::from_content(std::string(fiber::test::kQuicTestCertificatePem));
    auto context = fiber::net::TlsContext::create(material);
    ASSERT_TRUE(context);
    EXPECT_TRUE((*context)->has_identity());
    EXPECT_TRUE((*context)->has_trust_store());

    fiber::net::TlsClientConnectionOptions client_options{};
    client_options.context = context->get();
    client_options.verify_peer = true;
    client_options.sni_name = "localhost";
    auto client_ssl = (*context)->create_client_ssl(client_options);
    ASSERT_TRUE(client_ssl);
    SSL_free(*client_ssl);

    fiber::net::TlsServerConnectionOptions server_options{};
    server_options.default_context = context->get();
    auto server_ssl = fiber::net::TlsContext::create_server_ssl(server_options, nullptr, nullptr,
                                                                fiber::net::TlsTransportKind::Tcp);
    ASSERT_TRUE(server_ssl);
    SSL_free(*server_ssl);
}

} // namespace
