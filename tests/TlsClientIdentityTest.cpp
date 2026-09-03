#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <future>
#include <signal.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <fiber/async/Spawn.h>
#include <fiber/async/Task.h>
#include <fiber/common/IoError.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/HttpTransport.h>
#include <fiber/net/SocketAddress.h>
#include <fiber/net/TcpListener.h>
#include <fiber/net/TlsCredential.h>
#include <fiber/net/TlsServerHandshakeConfig.h>
#include <fiber/net/TrustStore.h>

#include "QuicTestTlsCertificate.h"
#include "TlsClientIdentityTestData.h"

namespace {

using fiber::async::DetachedTask;
using namespace std::chrono_literals;

std::string make_temp_path(const char *tag) {
    static std::atomic_uint64_t sequence{0};
    std::string path = "/tmp/fiber_tls_client_identity_";
    path.append(tag);
    path.push_back('_');
    path.append(std::to_string(static_cast<long>(::getpid())));
    path.push_back('_');
    path.append(std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
    path.append(".pem");
    return path;
}

struct TempPemFile {
    std::string path;
    bool ok = false;

    TempPemFile(const char *tag, std::string_view first, std::string_view second = {}) : path(make_temp_path(tag)) {
        std::ofstream out(path, std::ios::binary);
        if (out) {
            out.write(first.data(), static_cast<std::streamsize>(first.size()));
            out.write(second.data(), static_cast<std::streamsize>(second.size()));
            ok = out.good();
        }
        if (!ok) {
            ::unlink(path.c_str());
            path.clear();
        }
    }

    ~TempPemFile() {
        if (!path.empty()) {
            ::unlink(path.c_str());
        }
    }
};

struct SigpipeGuard {
    using Handler = void (*)(int);

    Handler previous = SIG_DFL;

    SigpipeGuard() { previous = ::signal(SIGPIPE, SIG_IGN); }
    ~SigpipeGuard() { (void) ::signal(SIGPIPE, previous); }
};

struct IdentityFiles {
    TempPemFile root{"root", fiber::test::kRootCertPem};
    TempPemFile alternate_root{"alternate_root", fiber::test::kQuicTestCertificatePem};
    TempPemFile server_chain{"server_chain", fiber::test::kServerCertPem, fiber::test::kIntermediateCertPem};
    TempPemFile server_key{"server_key", fiber::test::kServerKeyPem};
    TempPemFile client_chain{"client_chain", fiber::test::kClientCertPem, fiber::test::kIntermediateCertPem};
    TempPemFile client_key{"client_key", fiber::test::kClientKeyPem};
    TempPemFile wrong_key{"wrong_key", fiber::test::kWrongKeyPem};

    [[nodiscard]] bool ok() const noexcept {
        return root.ok && alternate_root.ok && server_chain.ok && server_key.ok && client_chain.ok && client_key.ok &&
               wrong_key.ok;
    }
};

struct SelectorState {
    std::array<char, 256> server_name{};
    std::size_t server_name_size = 0;
    bool saw_test_alpn = false;
};

struct IdentityTls {
    std::unique_ptr<fiber::net::TlsCredential> credential;
    std::unique_ptr<fiber::net::TrustStore> trust_store;
};

struct ServerCallbackState {
    const fiber::net::TlsCredential *credential = nullptr;
    SelectorState *selector = nullptr;
};

fiber::common::IoErr capture_client_hello(void *ctx, fiber::net::TlsServerHandshakeConfig &config,
                                          const fiber::net::TlsClientHelloView &input) noexcept {
    auto *state = static_cast<ServerCallbackState *>(ctx);
    if (!state || !state->credential) {
        return fiber::common::IoErr::Invalid;
    }
    if (state->selector) {
        SelectorState &selector = *state->selector;
        selector.server_name_size = std::min(input.server_name.size(), selector.server_name.size());
        if (selector.server_name_size != 0) {
            std::memcpy(selector.server_name.data(), input.server_name.data(), selector.server_name_size);
        }
        selector.saw_test_alpn = input.offered_alpn.contains("fiber-mtls-test");
    }
    return config.add_credential(*state->credential);
}

fiber::common::IoErr reject_server_configuration(void *, fiber::net::TlsServerHandshakeConfig &,
                                                 const fiber::net::TlsClientHelloView &) noexcept {
    return fiber::common::IoErr::Permission;
}

struct HandshakeResult {
    fiber::common::IoErr server_err = fiber::common::IoErr::Unknown;
    fiber::common::IoErr client_err = fiber::common::IoErr::Unknown;
    std::string server_alpn;
    std::string client_alpn;
    bool completed = false;
    bool transports_released = false;
};

struct HandshakeSideResult {
    fiber::common::IoErr err = fiber::common::IoErr::Unknown;
    std::string alpn;
};

DetachedTask run_handshake(fiber::http::TlsTransport *transport, std::chrono::milliseconds timeout,
                           std::promise<HandshakeSideResult> *done) {
    HandshakeSideResult result;
    auto handshake_result = co_await transport->handshake(timeout);
    result.err = handshake_result ? fiber::common::IoErr::None : handshake_result.error();
    if (handshake_result) {
        result.alpn.assign(transport->negotiated_alpn());
    }
    done->set_value(std::move(result));
    co_return;
}

DetachedTask destroy_transport(fiber::http::TlsTransport *transport, std::promise<void> *done) {
    transport->close();
    delete transport;
    done->set_value();
    co_return;
}

void destroy_on_owner(fiber::event::EventLoop &loop, fiber::http::TlsTransport *transport) {
    std::promise<void> promise;
    auto future = promise.get_future();
    fiber::async::spawn(loop, [transport, &promise]() { return destroy_transport(transport, &promise); });
    future.wait();
}

HandshakeResult run_handshake_pair(const fiber::net::TlsServerParam &server_options,
                                   const fiber::net::TlsClientParam &client_options) {
    SigpipeGuard sigpipe_guard;
    HandshakeResult result;
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) != 0) {
        return result;
    }

    fiber::event::EventLoopGroup group(2);
    group.start();

    fiber::net::SocketAddress peer(fiber::net::IpAddress::loopback_v4(), 0);
    auto server_result =
            fiber::http::TlsTransport::create(group.at(0), fiber::net::AcceptResult(fds[0], peer), server_options);
    fds[0] = -1;
    auto client_result =
            fiber::http::TlsTransport::create(group.at(1), fiber::net::AcceptResult(fds[1], peer), client_options);
    fds[1] = -1;
    if (!server_result || !client_result) {
        if (server_result) {
            destroy_on_owner(group.at(0), server_result->release());
        }
        if (client_result) {
            destroy_on_owner(group.at(1), client_result->release());
        }
        group.stop();
        group.join();
        return result;
    }

    auto *server_transport = server_result->release();
    auto *client_transport = client_result->release();
    std::promise<HandshakeSideResult> server_promise;
    std::promise<HandshakeSideResult> client_promise;
    auto server_future = server_promise.get_future();
    auto client_future = client_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return run_handshake(server_transport, 2s, &server_promise); });
    fiber::async::spawn(group.at(1), [&]() { return run_handshake(client_transport, 2s, &client_promise); });

    auto server_side = server_future.get();
    auto client_side = client_future.get();
    result.server_err = server_side.err;
    result.client_err = client_side.err;
    result.server_alpn = std::move(server_side.alpn);
    result.client_alpn = std::move(client_side.alpn);
    result.completed = true;

    destroy_on_owner(group.at(0), server_transport);
    destroy_on_owner(group.at(1), client_transport);
    group.stop();
    group.join();
    result.transports_released = true;
    return result;
}

fiber::common::IoResult<IdentityTls> make_server_material(const IdentityFiles &files, std::string trust_path = {}) {
    fiber::net::TlsCredentialOptions credential_options{};
    credential_options.certificate_chain = fiber::net::TlsPemSource::from_file(files.server_chain.path);
    credential_options.private_key = fiber::net::TlsPemSource::from_file(files.server_key.path);
    auto credential = fiber::net::TlsCredential::create(credential_options);
    if (!credential) {
        return std::unexpected(credential.error());
    }
    auto trust_store = fiber::net::TrustStore::create(
            fiber::net::TrustStoreOptions::from_file(trust_path.empty() ? files.root.path : std::move(trust_path)));
    if (!trust_store) {
        return std::unexpected(trust_store.error());
    }
    return IdentityTls{.credential = std::move(*credential), .trust_store = std::move(*trust_store)};
}

fiber::common::IoResult<IdentityTls> make_client_material(const IdentityFiles &files, bool include_identity = true) {
    IdentityTls material{};
    if (include_identity) {
        fiber::net::TlsCredentialOptions credential_options{};
        credential_options.certificate_chain = fiber::net::TlsPemSource::from_file(files.client_chain.path);
        credential_options.private_key = fiber::net::TlsPemSource::from_file(files.client_key.path);
        auto credential = fiber::net::TlsCredential::create(credential_options);
        if (!credential) {
            return std::unexpected(credential.error());
        }
        material.credential = std::move(*credential);
    }
    auto trust_store = fiber::net::TrustStore::create(fiber::net::TrustStoreOptions::from_file(files.root.path));
    if (!trust_store) {
        return std::unexpected(trust_store.error());
    }
    material.trust_store = std::move(*trust_store);
    return material;
}

fiber::net::TlsServerParam make_server_options(const IdentityTls &material, ServerCallbackState &callback_state,
                                               SelectorState *selector_state) {
    callback_state.credential = material.credential.get();
    callback_state.selector = selector_state;
    fiber::net::TlsServerParam options{};
    options.configure_callback = &capture_client_hello;
    options.configure_ctx = &callback_state;
    options.trust_store = material.trust_store.get();
    options.client_certificate_mode = fiber::net::TlsClientCertificateMode::Required;
    // Server preference order: clients offering only "fiber-mtls-test" force
    // the negotiation to walk past the preferred "server-default" entry.
    static constexpr std::string_view kServerDefaultAlpn[] = {"server-default", "fiber-mtls-test"};
    options.alpn = kServerDefaultAlpn;
    return options;
}

fiber::net::TlsClientParam make_client_options(const IdentityTls &material) {
    fiber::net::TlsClientParam options{};
    options.security.credential = material.credential.get();
    options.security.trust_store = material.trust_store.get();
    options.security.verify_peer = true;
    options.alpn = {"fiber-mtls-test"};
    return options;
}

TEST(TlsClientIdentityTest, ValidatesPairAndChainBeforePublishingCredential) {
    IdentityFiles files;
    ASSERT_TRUE(files.ok());

    fiber::net::TlsCredentialOptions cert_only_options{};
    cert_only_options.certificate_chain = fiber::net::TlsPemSource::from_file(files.client_chain.path);
    auto cert_only_result = fiber::net::TlsCredential::create(cert_only_options);
    ASSERT_FALSE(cert_only_result);
    EXPECT_EQ(cert_only_result.error(), fiber::common::IoErr::Invalid);

    fiber::net::TlsCredentialOptions key_only_options{};
    key_only_options.private_key = fiber::net::TlsPemSource::from_file(files.client_key.path);
    auto key_only_result = fiber::net::TlsCredential::create(key_only_options);
    ASSERT_FALSE(key_only_result);
    EXPECT_EQ(key_only_result.error(), fiber::common::IoErr::Invalid);

    fiber::net::TlsCredentialOptions mismatch_options{};
    mismatch_options.certificate_chain = fiber::net::TlsPemSource::from_file(files.client_chain.path);
    mismatch_options.private_key = fiber::net::TlsPemSource::from_file(files.wrong_key.path);
    auto mismatch_result = fiber::net::TlsCredential::create(mismatch_options);
    ASSERT_FALSE(mismatch_result);
    EXPECT_EQ(mismatch_result.error(), fiber::common::IoErr::Invalid);
    EXPECT_EQ(ERR_peek_error(), 0U);

    fiber::net::TlsCredentialOptions missing_options{};
    missing_options.certificate_chain = fiber::net::TlsPemSource::from_file("/does-not-exist/client-chain.pem");
    missing_options.private_key = fiber::net::TlsPemSource::from_file("/does-not-exist/client-key.pem");
    auto missing_result = fiber::net::TlsCredential::create(missing_options);
    ASSERT_FALSE(missing_result);
    EXPECT_EQ(missing_result.error(), fiber::common::IoErr::Invalid);
    EXPECT_EQ(ERR_peek_error(), 0U);

    auto valid_material = make_client_material(files);
    ASSERT_TRUE(valid_material);
}

TEST(TlsClientIdentityTest, ServerConfigurationCallbackErrorIsReturnedByHandshake) {
    fiber::net::TlsServerParam server_options{};
    server_options.configure_callback = &reject_server_configuration;
    fiber::net::TlsClientParam client_options{};

    auto result = run_handshake_pair(server_options, client_options);
    ASSERT_TRUE(result.completed);
    EXPECT_EQ(result.server_err, fiber::common::IoErr::Permission);
    EXPECT_TRUE(result.transports_released);
}

TEST(TlsClientIdentityTest, MtlsComposesWithPeerVerificationIndependentNameSniAndAlpn) {
    IdentityFiles files;
    ASSERT_TRUE(files.ok());

    for (int version: {0x0303, 0x0304}) {
        SCOPED_TRACE(version);
        SelectorState selector_state;
        ServerCallbackState callback_state;

        auto server_material = make_server_material(files);
        ASSERT_TRUE(server_material);
        auto server_options = make_server_options(*server_material, callback_state, &selector_state);
        server_options.min_version = version;
        server_options.max_version = version;

        auto client_material = make_client_material(files);
        ASSERT_TRUE(client_material);
        auto client_options = make_client_options(*client_material);
        client_options.server_name = "routing.identity.test";
        client_options.verify_name = "server.identity.test";
        client_options.min_version = version;
        client_options.max_version = version;

        auto result = run_handshake_pair(server_options, client_options);
        ASSERT_TRUE(result.completed);
        EXPECT_EQ(result.server_err, fiber::common::IoErr::None);
        EXPECT_EQ(result.client_err, fiber::common::IoErr::None);
        EXPECT_EQ(result.server_alpn, "fiber-mtls-test");
        EXPECT_EQ(result.client_alpn, "fiber-mtls-test");
        EXPECT_EQ(std::string_view(selector_state.server_name.data(), selector_state.server_name_size),
                  "routing.identity.test");
        EXPECT_TRUE(selector_state.saw_test_alpn);
        EXPECT_TRUE(result.transports_released);
    }
}

TEST(TlsClientIdentityTest, MtlsIdentityWorksWhenPeerVerificationIsDisabled) {
    IdentityFiles files;
    ASSERT_TRUE(files.ok());
    SelectorState selector_state;
    ServerCallbackState callback_state;

    auto server_material = make_server_material(files);
    ASSERT_TRUE(server_material);
    auto server_options = make_server_options(*server_material, callback_state, &selector_state);

    auto client_material = make_client_material(files);
    ASSERT_TRUE(client_material);
    auto client_options = make_client_options(*client_material);
    client_options.security.verify_peer = false;
    client_options.server_name = "routing.identity.test";
    client_options.verify_name = "intentionally-wrong.identity.test";

    auto result = run_handshake_pair(server_options, client_options);
    ASSERT_TRUE(result.completed);
    EXPECT_EQ(result.server_err, fiber::common::IoErr::None);
    EXPECT_EQ(result.client_err, fiber::common::IoErr::None);
}

TEST(TlsClientIdentityTest, ServerRequiringClientIdentityRejectsAnonymousClient) {
    IdentityFiles files;
    ASSERT_TRUE(files.ok());
    SelectorState selector_state;
    ServerCallbackState callback_state;

    auto server_material = make_server_material(files);
    ASSERT_TRUE(server_material);
    auto server_options = make_server_options(*server_material, callback_state, &selector_state);

    auto client_material = make_client_material(files, false);
    ASSERT_TRUE(client_material);
    auto client_options = make_client_options(*client_material);
    client_options.server_name = "server.identity.test";
    client_options.alpn = {"fiber-mtls-test"};

    auto result = run_handshake_pair(server_options, client_options);
    ASSERT_TRUE(result.completed);
    EXPECT_NE(result.server_err, fiber::common::IoErr::None);
    EXPECT_TRUE(result.transports_released);
}

TEST(TlsClientIdentityTest, ServerRejectsClientSignedByUnknownCaAndReleasesTransports) {
    IdentityFiles files;
    ASSERT_TRUE(files.ok());
    SelectorState selector_state;
    ServerCallbackState callback_state;

    auto server_material = make_server_material(files, files.alternate_root.path);
    ASSERT_TRUE(server_material);
    auto server_options = make_server_options(*server_material, callback_state, &selector_state);

    auto client_material = make_client_material(files);
    ASSERT_TRUE(client_material);
    auto client_options = make_client_options(*client_material);
    client_options.server_name = "server.identity.test";

    auto result = run_handshake_pair(server_options, client_options);
    ASSERT_TRUE(result.completed);
    EXPECT_NE(result.server_err, fiber::common::IoErr::None);
    EXPECT_TRUE(result.transports_released);
}

TEST(TlsClientIdentityTest, FailedClientHandshakeCanBeReleasedOnOwnerLoop) {
    SigpipeGuard sigpipe_guard;
    IdentityFiles files;
    ASSERT_TRUE(files.ok());
    auto client_material = make_client_material(files);
    ASSERT_TRUE(client_material);
    auto client_options = make_client_options(*client_material);
    client_options.security.verify_peer = false;

    int fds[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::net::SocketAddress peer(fiber::net::IpAddress::loopback_v4(), 0);
    auto transport_result =
            fiber::http::TlsTransport::create(group.at(0), fiber::net::AcceptResult(fds[0], peer), client_options);
    fds[0] = -1;
    ASSERT_TRUE(transport_result);
    auto *transport = transport_result->release();

    std::promise<HandshakeSideResult> promise;
    auto future = promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return run_handshake(transport, 20ms, &promise); });
    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(future.get().err, fiber::common::IoErr::TimedOut);

    destroy_on_owner(group.at(0), transport);
    ::close(fds[1]);
    group.stop();
    group.join();
}

} // namespace
