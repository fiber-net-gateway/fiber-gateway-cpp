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
#include <fiber/net/TlsContext.h>
#include <fiber/net/TlsOptions.h>

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

fiber::net::TlsContext *capture_client_hello(void *ctx, const fiber::net::TlsIdentitySelectInput &input) noexcept {
    auto *state = static_cast<SelectorState *>(ctx);
    if (!state) {
        return nullptr;
    }
    state->server_name_size = std::min(input.server_name.size(), state->server_name.size());
    if (state->server_name_size != 0) {
        std::memcpy(state->server_name.data(), input.server_name.data(), state->server_name_size);
    }
    state->saw_test_alpn = input.alpn.contains("fiber-mtls-test");
    return nullptr;
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

HandshakeResult run_handshake_pair(fiber::net::TlsServerContext &server_context,
                                   fiber::net::TlsContext &client_context) {
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
            fiber::http::TlsTransport::create(group.at(0), fiber::net::AcceptResult(fds[0], peer), server_context);
    fds[0] = -1;
    auto client_result =
            fiber::http::TlsTransport::create(group.at(1), fiber::net::AcceptResult(fds[1], peer), client_context);
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

fiber::net::TlsOptions make_server_options(const IdentityFiles &files, SelectorState *selector_state) {
    fiber::net::TlsOptions options{};
    options.cert_file = files.server_chain.path;
    options.key_file = files.server_key.path;
    options.ca_file = files.root.path;
    options.verify_client = true;
    options.alpn = {"fiber-mtls-test"};
    options.identity_selector_ops = {
            .select = &capture_client_hello,
            .ctx = selector_state,
    };
    return options;
}

fiber::net::TlsOptions make_client_options(const IdentityFiles &files) {
    fiber::net::TlsOptions options{};
    options.cert_file = files.client_chain.path;
    options.key_file = files.client_key.path;
    options.alpn = {"fiber-mtls-test"};
    return options;
}

TEST(TlsClientIdentityTest, ValidatesPairAndChainBeforePublishingContext) {
    IdentityFiles files;
    ASSERT_TRUE(files.ok());

    fiber::net::TlsOptions empty_options{};
    fiber::net::TlsContext empty_context(std::move(empty_options), false);
    ASSERT_TRUE(empty_context.init());
    ASSERT_NE(empty_context.raw(), nullptr);

    fiber::net::TlsOptions cert_only_options{};
    cert_only_options.cert_file = files.client_chain.path;
    fiber::net::TlsContext cert_only_context(std::move(cert_only_options), false);
    auto cert_only_result = cert_only_context.init();
    ASSERT_FALSE(cert_only_result);
    EXPECT_EQ(cert_only_result.error(), fiber::common::IoErr::Invalid);
    EXPECT_EQ(cert_only_context.raw(), nullptr);

    fiber::net::TlsOptions key_only_options{};
    key_only_options.key_file = files.client_key.path;
    fiber::net::TlsContext key_only_context(std::move(key_only_options), false);
    auto key_only_result = key_only_context.init();
    ASSERT_FALSE(key_only_result);
    EXPECT_EQ(key_only_result.error(), fiber::common::IoErr::Invalid);
    EXPECT_EQ(key_only_context.raw(), nullptr);

    fiber::net::TlsOptions mismatch_options{};
    mismatch_options.cert_file = files.client_chain.path;
    mismatch_options.key_file = files.wrong_key.path;
    fiber::net::TlsContext mismatch_context(std::move(mismatch_options), false);
    auto mismatch_result = mismatch_context.init();
    ASSERT_FALSE(mismatch_result);
    EXPECT_EQ(mismatch_result.error(), fiber::common::IoErr::Invalid);
    EXPECT_EQ(mismatch_context.raw(), nullptr);
    EXPECT_EQ(ERR_peek_error(), 0U);

    fiber::net::TlsOptions missing_options{};
    missing_options.cert_file = "/does-not-exist/client-chain.pem";
    missing_options.key_file = "/does-not-exist/client-key.pem";
    fiber::net::TlsContext missing_context(std::move(missing_options), false);
    auto missing_result = missing_context.init();
    ASSERT_FALSE(missing_result);
    EXPECT_EQ(missing_result.error(), fiber::common::IoErr::Invalid);
    EXPECT_EQ(missing_context.raw(), nullptr);
    EXPECT_EQ(ERR_peek_error(), 0U);

    fiber::net::TlsOptions valid_options = make_client_options(files);
    fiber::net::TlsContext valid_context(std::move(valid_options), false);
    ASSERT_TRUE(valid_context.init());
    STACK_OF(X509) *chain = nullptr;
    ASSERT_EQ(SSL_CTX_get0_chain_certs(valid_context.raw(), &chain), 1);
    ASSERT_NE(chain, nullptr);
    EXPECT_EQ(sk_X509_num(chain), 1U);
}

TEST(TlsClientIdentityTest, MtlsComposesWithPeerVerificationIndependentNameSniAndAlpn) {
    IdentityFiles files;
    ASSERT_TRUE(files.ok());

    for (int version: {0x0303, 0x0304}) {
        SCOPED_TRACE(version);
        SelectorState selector_state;

        auto server_options = make_server_options(files, &selector_state);
        server_options.min_version = version;
        server_options.max_version = version;
        fiber::net::TlsServerContext server_context(std::move(server_options));
        ASSERT_TRUE(server_context.init());

        auto client_options = make_client_options(files);
        client_options.ca_file = files.root.path;
        client_options.verify_peer = true;
        client_options.server_name = "routing.identity.test";
        client_options.verify_name = "server.identity.test";
        client_options.min_version = version;
        client_options.max_version = version;
        fiber::net::TlsContext client_context(std::move(client_options), false);
        ASSERT_TRUE(client_context.init());

        auto result = run_handshake_pair(server_context, client_context);
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

    auto server_options = make_server_options(files, &selector_state);
    fiber::net::TlsServerContext server_context(std::move(server_options));
    ASSERT_TRUE(server_context.init());

    auto client_options = make_client_options(files);
    client_options.verify_peer = false;
    client_options.server_name = "routing.identity.test";
    client_options.verify_name = "intentionally-wrong.identity.test";
    fiber::net::TlsContext client_context(std::move(client_options), false);
    ASSERT_TRUE(client_context.init());

    auto result = run_handshake_pair(server_context, client_context);
    ASSERT_TRUE(result.completed);
    EXPECT_EQ(result.server_err, fiber::common::IoErr::None);
    EXPECT_EQ(result.client_err, fiber::common::IoErr::None);
}

TEST(TlsClientIdentityTest, ServerRequiringClientIdentityRejectsAnonymousClient) {
    IdentityFiles files;
    ASSERT_TRUE(files.ok());
    SelectorState selector_state;

    auto server_options = make_server_options(files, &selector_state);
    fiber::net::TlsServerContext server_context(std::move(server_options));
    ASSERT_TRUE(server_context.init());

    fiber::net::TlsOptions client_options{};
    client_options.ca_file = files.root.path;
    client_options.verify_peer = true;
    client_options.server_name = "server.identity.test";
    client_options.alpn = {"fiber-mtls-test"};
    fiber::net::TlsContext client_context(std::move(client_options), false);
    ASSERT_TRUE(client_context.init());

    auto result = run_handshake_pair(server_context, client_context);
    ASSERT_TRUE(result.completed);
    EXPECT_NE(result.server_err, fiber::common::IoErr::None);
    EXPECT_TRUE(result.transports_released);
}

TEST(TlsClientIdentityTest, ServerRejectsClientSignedByUnknownCaAndReleasesTransports) {
    IdentityFiles files;
    ASSERT_TRUE(files.ok());
    SelectorState selector_state;

    auto server_options = make_server_options(files, &selector_state);
    server_options.ca_file = files.alternate_root.path;
    fiber::net::TlsServerContext server_context(std::move(server_options));
    ASSERT_TRUE(server_context.init());

    auto client_options = make_client_options(files);
    client_options.ca_file = files.root.path;
    client_options.verify_peer = true;
    client_options.server_name = "server.identity.test";
    fiber::net::TlsContext client_context(std::move(client_options), false);
    ASSERT_TRUE(client_context.init());

    auto result = run_handshake_pair(server_context, client_context);
    ASSERT_TRUE(result.completed);
    EXPECT_NE(result.server_err, fiber::common::IoErr::None);
    EXPECT_TRUE(result.transports_released);
}

TEST(TlsClientIdentityTest, FailedClientHandshakeCanBeReleasedOnOwnerLoop) {
    SigpipeGuard sigpipe_guard;
    IdentityFiles files;
    ASSERT_TRUE(files.ok());
    auto client_options = make_client_options(files);
    fiber::net::TlsContext client_context(std::move(client_options), false);
    ASSERT_TRUE(client_context.init());

    int fds[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::net::SocketAddress peer(fiber::net::IpAddress::loopback_v4(), 0);
    auto transport_result =
            fiber::http::TlsTransport::create(group.at(0), fiber::net::AcceptResult(fds[0], peer), client_context);
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
