#include <gtest/gtest.h>

#include <memory>
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

struct ReleasingCredentialState {
    std::unique_ptr<fiber::net::TlsCredential> *credential = nullptr;
};

fiber::common::IoErr add_and_release_credential(void *ctx, fiber::net::TlsServerHandshakeConfig &config,
                                                const fiber::net::TlsClientHelloView &) noexcept {
    auto *state = static_cast<ReleasingCredentialState *>(ctx);
    if (!state || !state->credential || !*state->credential) {
        return fiber::common::IoErr::Invalid;
    }
    fiber::common::IoErr error = config.add_credential(**state->credential);
    if (error == fiber::common::IoErr::None) {
        state->credential->reset();
    }
    return error;
}

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

TEST(TlsCredentialTest, SslRetainsCredentialAfterOwnerReferenceIsReleased) {
    fiber::net::TlsCredentialOptions credential_options{};
    credential_options.certificate_chain =
            fiber::net::TlsPemSource::from_content(std::string(fiber::test::kQuicTestCertificatePem));
    credential_options.private_key =
            fiber::net::TlsPemSource::from_content(std::string(fiber::test::kQuicTestPrivateKeyPem));
    auto credential = fiber::net::TlsCredential::create(credential_options);
    ASSERT_TRUE(credential.has_value());

    ReleasingCredentialState callback_state{.credential = &*credential};
    fiber::net::TlsServerParam server_param{};
    server_param.configure_callback = &add_and_release_credential;
    server_param.configure_ctx = &callback_state;
    fiber::net::detail::TlsServerHandshakeState server_state{};
    auto server_result = fiber::net::detail::TlsSslFactory::create_server(server_param, server_state, nullptr, nullptr,
                                                                          fiber::net::TlsTransportKind::Tcp);
    ASSERT_TRUE(server_result.has_value());
    bssl::UniquePtr<SSL> server(*server_result);

    fiber::net::TlsClientParam client_param{};
    client_param.enable_tls = true;
    auto client_result = fiber::net::detail::TlsSslFactory::create_client(client_param, nullptr);
    ASSERT_TRUE(client_result.has_value());
    bssl::UniquePtr<SSL> client(*client_result);

    BIO *client_bio = nullptr;
    BIO *server_bio = nullptr;
    ASSERT_EQ(BIO_new_bio_pair(&client_bio, 0, &server_bio, 0), 1);
    SSL_set_bio(client.get(), client_bio, client_bio);
    SSL_set_bio(server.get(), server_bio, server_bio);

    bool client_done = false;
    bool server_done = false;
    for (int i = 0; i < 100 && (!client_done || !server_done); ++i) {
        if (!client_done) {
            const int rc = SSL_do_handshake(client.get());
            client_done = rc == 1;
            if (!client_done) {
                const int error = SSL_get_error(client.get(), rc);
                ASSERT_TRUE(error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE);
            }
        }
        if (!server_done) {
            const int rc = SSL_do_handshake(server.get());
            server_done = rc == 1;
            if (!server_done) {
                const int error = SSL_get_error(server.get(), rc);
                ASSERT_TRUE(error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE);
            }
        }
    }

    EXPECT_EQ(*credential, nullptr);
    EXPECT_TRUE(client_done);
    EXPECT_TRUE(server_done);
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
