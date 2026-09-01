#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <future>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include <openssl/hmac.h>
#include <openssl/ssl.h>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/net/TlsContext.h>
#include <fiber/net/UdpSocket.h>
#include <fiber/quic/QuicClient.h>
#include <fiber/quic/QuicUdpEndpoint.h>
#include "QuicTestTlsCertificate.h"
#include "TlsClientIdentityTestData.h"

namespace {

using namespace std::chrono_literals;

void destroy_connection(void *, fiber::quic::QuicConnection &connection) noexcept { delete &connection; }

void destroy_stream(void *, fiber::quic::QuicStream &stream) noexcept { delete &stream; }

fiber::quic::QuicStream::Lease create_stream(void *, std::uint64_t) noexcept {
    return fiber::quic::QuicStream::Lease::adopt(new (std::nothrow) fiber::quic::QuicStream(nullptr, destroy_stream));
}

fiber::quic::QuicConnection::Lease
create_server_connection(void *owner, const fiber::quic::QuicConnection::Options &options) noexcept {
    fiber::quic::QuicConnection::Options owned = options;
    owned.on_destroy = destroy_connection;
    owned.owner = owner;
    owned.ops.create_stream = create_stream;
    return fiber::quic::QuicConnection::Lease::adopt(new (std::nothrow) fiber::quic::QuicConnection(owned));
}

fiber::quic::QuicConnection::Lease create_connection(void *,
                                                     const fiber::quic::QuicConnection::Options &options) noexcept {
    fiber::quic::QuicConnection::Options owned = options;
    owned.on_destroy = destroy_connection;
    return fiber::quic::QuicConnection::Lease::adopt(new (std::nothrow) fiber::quic::QuicConnection(owned));
}

fiber::common::IoResult<std::unique_ptr<fiber::net::TlsContext>>
create_tls_context(std::string_view certificate = {}, std::string_view private_key = {}, std::string_view trust = {}) {
    fiber::net::TlsOptions material{};
    if (!certificate.empty()) {
        material.certificate_chain = fiber::net::TlsPemSource::from_file(std::string(certificate));
        material.private_key = fiber::net::TlsPemSource::from_file(std::string(private_key));
    }
    if (!trust.empty()) {
        material.trust_store = fiber::net::TlsTrustStoreSource::from_file(std::string(trust));
    }
    return fiber::net::TlsContext::create(material);
}

fiber::net::TlsClientConnectionOptions make_quic_client_tls(const fiber::net::TlsContext *context,
                                                            bool verify_peer = true) {
    return {
            .context = context,
            .verify_peer = verify_peer,
            .min_version = 0x0304,
            .max_version = 0x0304,
            .alpn = {"fiber-quic-test"},
    };
}

fiber::net::TlsServerConnectionOptions make_quic_server_tls(
        const fiber::net::TlsContext *context,
        fiber::net::TlsClientCertificateMode client_certificate_mode = fiber::net::TlsClientCertificateMode::None) {
    return {
            .default_context = context,
            .client_certificate_mode = client_certificate_mode,
            .min_version = 0x0304,
            .max_version = 0x0304,
            .alpn = {"fiber-quic-test"},
    };
}

struct StartSummary {
    fiber::common::IoErr error = fiber::common::IoErr::None;
    fiber::quic::QuicConnectionState state = fiber::quic::QuicConnectionState::Closed;
    std::size_t endpoint_connections = 0;
    bool tls_initialized = false;
    bool initial_keys_ready = false;
    bool cid_registered = false;
    bool initial_output_queued = false;
};

struct TimeoutSummary {
    fiber::common::IoErr error = fiber::common::IoErr::None;
    fiber::quic::QuicConnectPhase phase = fiber::quic::QuicConnectPhase::Handshake;
    std::size_t endpoint_connections = 0;
};

fiber::async::DetachedTask start_client_attempt(fiber::quic::QuicUdpEndpoint *endpoint, fiber::quic::QuicClient *client,
                                                std::promise<StartSummary> *promise) {
    StartSummary summary{};
    auto started_endpoint = endpoint->start();
    if (!started_endpoint) {
        summary.error = started_endpoint.error();
        promise->set_value(summary);
        co_return;
    }

    fiber::quic::QuicClientConnectOptions options{};
    options.remote_addr = {fiber::net::IpAddress::loopback_v4(), 4433};
    options.server_name = "localhost";
    options.allow_insecure = true;
    auto started = client->start_connect(options);
    if (!started) {
        summary.error = started.error().io_error;
        endpoint->close();
        promise->set_value(summary);
        co_return;
    }

    fiber::quic::QuicClientAttempt attempt = std::move(*started);
    fiber::quic::QuicConnection *connection = attempt.connection();
    summary.state = connection->state();
    summary.endpoint_connections = endpoint->active_connection_count();
    summary.tls_initialized = connection->tls().initialized();
    summary.initial_keys_ready = connection->crypto().initial_ready();
    summary.cid_registered = endpoint->find_connection(connection->local_connection_id()) == connection;
    summary.initial_output_queued =
            !connection->packet_number_space(fiber::quic::QuicEncryptionLevel::Initial).pending_frames.empty();

    endpoint->close();
    promise->set_value(summary);
}

fiber::async::DetachedTask timeout_client_attempt(fiber::quic::QuicUdpEndpoint *endpoint,
                                                  fiber::quic::QuicClient *client,
                                                  std::promise<TimeoutSummary> *promise) {
    TimeoutSummary summary{};
    auto started = endpoint->start();
    if (!started) {
        summary.error = started.error();
        promise->set_value(summary);
        co_return;
    }

    fiber::net::UdpSocket blackhole(fiber::event::EventLoop::current());
    auto bound = blackhole.bind({fiber::net::IpAddress::loopback_v4(), 0}, {});
    if (!bound) {
        summary.error = bound.error();
        endpoint->close();
        promise->set_value(summary);
        co_return;
    }

    fiber::quic::QuicClientConnectOptions options{};
    options.remote_addr = blackhole.local_addr();
    options.server_name = "localhost";
    options.allow_insecure = true;
    options.handshake_timeout = 10ms;
    auto connected = co_await client->connect(options);
    if (connected) {
        connected->reset();
    } else {
        summary.error = connected.error().io_error;
        summary.phase = connected.error().phase;
    }
    for (std::uint8_t attempt = 0; attempt < 50 && endpoint->active_connection_count() != 0; ++attempt) {
        co_await fiber::async::sleep(1ms);
    }
    summary.endpoint_connections = endpoint->active_connection_count();
    blackhole.close();
    endpoint->close();
    promise->set_value(summary);
}

struct ConnectSummary {
    fiber::common::IoErr error = fiber::common::IoErr::None;
    fiber::quic::QuicConnectPhase phase = fiber::quic::QuicConnectPhase::Handshake;
    long tls_verify_result = 0;
    std::uint8_t tls_alert = 0;
    fiber::quic::QuicConnectionState state = fiber::quic::QuicConnectionState::Closed;
    std::string selected_alpn{};
    bool peer_transport_received = false;
    bool server_scid_adopted = false;
    bool retry_processed = false;
    std::size_t client_endpoint_connections = 0;
    std::size_t server_endpoint_connections = 0;
};

enum class QuicClientIdentityMode : std::uint8_t {
    Trusted,
    Anonymous,
    UnknownCa,
};

struct QuicMtlsCase {
    const char *name = nullptr;
    QuicClientIdentityMode identity = QuicClientIdentityMode::Anonymous;
    bool expect_success = false;
};

class QuicClientMtlsTest : public ::testing::TestWithParam<QuicMtlsCase> {};

struct TestClientCache {
    ~TestClientCache() {
        if (session != nullptr) {
            SSL_SESSION_free(session);
        }
    }

    static bool load(void *owner, const fiber::quic::QuicClientCacheKey &,
                     fiber::quic::QuicClientCachedState &out) noexcept {
        auto &cache = *static_cast<TestClientCache *>(owner);
        ++cache.load_count;
        if (cache.session == nullptr) {
            return false;
        }
        out.session = cache.session;
        out.token = cache.token.empty() ? nullptr : cache.token.data();
        out.token_len = cache.token.size();
        out.remembered_transport = cache.remembered_transport;
        out.has_remembered_transport = true;
        return true;
    }

    static bool store_session(void *owner, const fiber::quic::QuicClientCacheKey &, SSL_SESSION *session,
                              const fiber::quic::QuicTransportSettings &remembered) noexcept {
        auto &cache = *static_cast<TestClientCache *>(owner);
        if (cache.session != nullptr) {
            SSL_SESSION_free(cache.session);
        }
        cache.session = session;
        cache.remembered_transport = remembered;
        ++cache.session_store_count;
        return true;
    }

    static void store_token(void *owner, const fiber::quic::QuicClientCacheKey &, const std::uint8_t *token,
                            std::size_t token_len) noexcept {
        auto &cache = *static_cast<TestClientCache *>(owner);
        cache.token.assign(token, token + token_len);
        ++cache.token_store_count;
    }

    SSL_SESSION *session = nullptr;
    std::vector<std::uint8_t> token{};
    fiber::quic::QuicTransportSettings remembered_transport{};
    std::size_t load_count = 0;
    std::size_t session_store_count = 0;
    std::size_t token_store_count = 0;
};

struct ResumptionSummary {
    fiber::common::IoErr error = fiber::common::IoErr::None;
    bool session_cached = false;
    bool token_cached = false;
    bool session_reused = false;
    bool token_reused = false;
    bool early_data_attempted = false;
    bool early_data_accepted = false;
    bool early_stream_queued = false;
    bool early_write_ready = false;
    bool cached_session_early_capable = false;
    fiber::common::IoErr early_attach_error = fiber::common::IoErr::None;
};

struct StatelessResetSummary {
    fiber::common::IoErr error = fiber::common::IoErr::None;
    fiber::quic::QuicConnectionState state = fiber::quic::QuicConnectionState::Closed;
    fiber::quic::QuicCloseSource close_source = fiber::quic::QuicCloseSource::None;
    bool token_installed = false;
};

bool create_reset_token(const std::array<std::uint8_t, fiber::quic::kQuicStatelessResetSecretLength> &secret,
                        const fiber::quic::QuicConnectionId &cid,
                        std::uint8_t out[fiber::quic::kStatelessResetTokenLength]) noexcept {
    std::uint8_t message[1 + fiber::quic::kMaxConnectionIdLength]{};
    message[0] = cid.length;
    std::memcpy(message + 1, cid.data(), cid.size());
    std::uint8_t digest[32]{};
    unsigned int digest_len = 0;
    if (HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()), message, 1 + cid.size(), digest,
             &digest_len) == nullptr ||
        digest_len < fiber::quic::kStatelessResetTokenLength) {
        return false;
    }
    std::memcpy(out, digest, fiber::quic::kStatelessResetTokenLength);
    return true;
}

fiber::async::DetachedTask receive_unknown_dcid_stateless_reset(
        fiber::quic::QuicUdpEndpoint *server_endpoint, fiber::quic::QuicUdpEndpoint *client_endpoint,
        fiber::quic::QuicClient *client,
        const std::array<std::uint8_t, fiber::quic::kQuicStatelessResetSecretLength> *secret,
        std::promise<StatelessResetSummary> *promise) {
    StatelessResetSummary summary{};
    auto server_started = server_endpoint->start();
    auto client_started = client_endpoint->start();
    if (!server_started || !client_started) {
        summary.error = !server_started ? server_started.error() : client_started.error();
        promise->set_value(summary);
        co_return;
    }

    fiber::quic::QuicClientConnectOptions options{};
    options.remote_addr = {fiber::net::IpAddress::loopback_v4(), server_endpoint->local_addr().port()};
    options.server_name = "localhost";
    auto connected = co_await client->connect(options);
    if (!connected) {
        summary.error = connected.error().io_error;
    } else {
        fiber::quic::QuicConnection *connection = connected->get();
        std::array<std::uint8_t, fiber::quic::kStatelessResetTokenLength> token{};
        std::array<std::uint8_t, 64> packet{};
        packet.fill(0x5a);
        packet[0] = fiber::quic::kPacketFlagFixed | 0x03U;
        if (!create_reset_token(*secret, connection->server_initial_source_connection_id(), token.data())) {
            summary.error = fiber::common::IoErr::Invalid;
        } else {
            std::memcpy(packet.data() + packet.size() - token.size(), token.data(), token.size());
            summary.token_installed = connection->detects_stateless_reset(packet.data(), packet.size());

            fiber::net::UdpSocket sender(fiber::event::EventLoop::current());
            auto bound = sender.bind({fiber::net::IpAddress::loopback_v4(), 0}, {});
            if (!bound) {
                summary.error = bound.error();
            } else {
                auto sent = co_await sender.send_to(packet.data(), packet.size(), client_endpoint->local_addr());
                if (!sent) {
                    summary.error = sent.error();
                } else {
                    co_await fiber::async::sleep(10ms);
                    summary.state = connection->state();
                    summary.close_source = connection->close_source();
                }
                sender.close();
            }
        }
    }

    client_endpoint->close();
    server_endpoint->close();
    if (connected) {
        connected->reset();
    }
    promise->set_value(summary);
}

fiber::async::DetachedTask connect_twice_with_cache(fiber::quic::QuicUdpEndpoint *server_endpoint,
                                                    fiber::quic::QuicUdpEndpoint *client_endpoint,
                                                    fiber::quic::QuicClient *client, TestClientCache *cache,
                                                    std::promise<ResumptionSummary> *promise) {
    ResumptionSummary summary{};
    auto server_started = server_endpoint->start();
    auto client_started = client_endpoint->start();
    if (!server_started || !client_started) {
        summary.error = !server_started ? server_started.error() : client_started.error();
        promise->set_value(summary);
        co_return;
    }

    fiber::quic::QuicClientConnectOptions options{};
    options.remote_addr = {fiber::net::IpAddress::loopback_v4(), server_endpoint->local_addr().port()};
    options.server_name = "localhost";
    options.handshake_timeout = 2s;
    auto first = co_await client->connect(options);
    if (!first) {
        summary.error = first.error().io_error;
    } else {
        (void) co_await (*first)->wait_confirmed(2s);
        co_await fiber::async::sleep(20ms);
        summary.session_cached = cache->session != nullptr;
        summary.token_cached = !cache->token.empty();
        (*first)->close_immediately();
        first->reset();
        co_await fiber::async::sleep(5ms);

        options.enable_early_data = true;
        auto second_started = client->start_connect(options);
        summary.cached_session_early_capable =
                cache->session != nullptr && SSL_SESSION_early_data_capable(cache->session) == 1;
        if (!second_started) {
            summary.error = second_started.error().io_error;
        } else {
            fiber::quic::QuicClientAttempt attempt = std::move(*second_started);
            fiber::quic::QuicConnection *connection = attempt.connection();
            summary.early_write_ready = connection->crypto().early_write().ready();
            auto stream = fiber::quic::QuicStream::Lease::adopt(
                    new (std::nothrow) fiber::quic::QuicStream(nullptr, destroy_stream));
            auto attached =
                    connection->try_attach_local_stream(std::move(stream), fiber::quic::QuicStreamType::Bidirectional,
                                                        fiber::quic::QuicStreamEarlyDataMode::ReplaySafe);
            if (attached) {
                fiber::mem::IoBuf data = fiber::mem::IoBuf::allocate(4);
                if (data) {
                    std::memcpy(data.writable_data(), "ping", 4);
                    data.commit(4);
                    summary.early_stream_queued = (*attached)->try_write(data, true).has_value();
                }
            } else {
                summary.early_attach_error = attached.error();
            }
            auto connected = co_await attempt.wait_connected(2s);
            if (!connected) {
                summary.error = connected.error().io_error;
            } else {
                summary.session_reused = connection->tls().session_reused();
                summary.token_reused = connection->initial_token().readable() == cache->token.size();
                summary.early_data_attempted = connection->early_data_attempted();
                summary.early_data_accepted = connection->early_data_accepted();
                fiber::quic::QuicConnection::Lease second = attempt.release();
                second->close_immediately();
                second.reset();
            }
        }
    }

    client_endpoint->close();
    server_endpoint->close();
    promise->set_value(summary);
}

fiber::async::DetachedTask connect_loopback(fiber::quic::QuicUdpEndpoint *server_endpoint,
                                            fiber::quic::QuicUdpEndpoint *client_endpoint,
                                            fiber::quic::QuicClient *client, const char *server_name,
                                            std::promise<ConnectSummary> *promise) {
    ConnectSummary summary{};
    auto server_started = server_endpoint->start();
    auto client_started = client_endpoint->start();
    if (!server_started || !client_started) {
        summary.error = !server_started ? server_started.error() : client_started.error();
        if (client_endpoint->valid()) {
            client_endpoint->close();
        }
        if (server_endpoint->valid()) {
            server_endpoint->close();
        }
        promise->set_value(std::move(summary));
        co_return;
    }

    fiber::quic::QuicClientConnectOptions options{};
    options.remote_addr = {fiber::net::IpAddress::loopback_v4(), server_endpoint->local_addr().port()};
    options.server_name = server_name;
    options.handshake_timeout = 2s;
    auto connected = co_await client->connect(options);
    if (!connected) {
        summary.error = connected.error().io_error;
        summary.phase = connected.error().phase;
        summary.tls_verify_result = connected.error().tls_verify_result;
        summary.tls_alert = connected.error().tls_alert;
    } else {
        fiber::quic::QuicConnection *connection = connected->get();
        summary.state = connection->state();
        summary.selected_alpn.assign(connection->tls().selected_alpn());
        summary.peer_transport_received = connection->peer_transport_params_received();
        summary.server_scid_adopted = connection->has_server_initial_source_connection_id();
        summary.retry_processed = connection->retry_processed();
    }

    client_endpoint->close();
    server_endpoint->close();
    if (connected) {
        connected->reset();
    }
    promise->set_value(std::move(summary));
}

fiber::async::DetachedTask connect_loopback_confirmed(fiber::quic::QuicUdpEndpoint *server_endpoint,
                                                      fiber::quic::QuicUdpEndpoint *client_endpoint,
                                                      fiber::quic::QuicClient *client, const char *server_name,
                                                      std::promise<ConnectSummary> *promise) {
    ConnectSummary summary{};
    auto server_started = server_endpoint->start();
    auto client_started = client_endpoint->start();
    if (!server_started || !client_started) {
        summary.error = !server_started ? server_started.error() : client_started.error();
        if (client_endpoint->valid()) {
            client_endpoint->close();
        }
        if (server_endpoint->valid()) {
            server_endpoint->close();
        }
        promise->set_value(std::move(summary));
        co_return;
    }

    fiber::quic::QuicClientConnectOptions options{};
    options.remote_addr = {fiber::net::IpAddress::loopback_v4(), server_endpoint->local_addr().port()};
    options.server_name = server_name;
    options.handshake_timeout = 2s;
    auto started = client->start_connect(options);
    if (!started) {
        summary.error = started.error().io_error;
        summary.phase = started.error().phase;
        summary.tls_verify_result = started.error().tls_verify_result;
        summary.tls_alert = started.error().tls_alert;
    } else {
        fiber::quic::QuicClientAttempt attempt = std::move(*started);
        auto confirmed = co_await attempt.wait_confirmed(2s);
        if (!confirmed) {
            summary.error = confirmed.error().io_error;
            summary.phase = confirmed.error().phase;
            summary.tls_verify_result = confirmed.error().tls_verify_result;
            summary.tls_alert = confirmed.error().tls_alert;
        } else {
            fiber::quic::QuicConnection *connection = attempt.connection();
            summary.state = connection->state();
            summary.selected_alpn.assign(connection->tls().selected_alpn());
            summary.peer_transport_received = connection->peer_transport_params_received();
            summary.server_scid_adopted = connection->has_server_initial_source_connection_id();
            summary.retry_processed = connection->retry_processed();
        }
        attempt.cancel();
    }

    client_endpoint->close();
    server_endpoint->close();
    summary.client_endpoint_connections = client_endpoint->active_connection_count();
    summary.server_endpoint_connections = server_endpoint->active_connection_count();
    promise->set_value(std::move(summary));
}

} // namespace

TEST(QuicClientTest, StartConnectAttachesAndQueuesClientInitial) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicUdpEndpoint endpoint;
    fiber::quic::QuicUdpEndpoint::EndpointOptions endpoint_options{};
    endpoint_options.bind_addr = {fiber::net::IpAddress::loopback_v4(), 0};
    ASSERT_TRUE(endpoint.init(group.at(0), endpoint_options));

    auto tls_context = create_tls_context();
    ASSERT_TRUE(tls_context);
    auto tls_options = make_quic_client_tls(tls_context->get(), false);

    fiber::quic::QuicClient client;
    ASSERT_TRUE(
            client.init(endpoint, tls_options, {.connection_owner = nullptr, .create_connection = create_connection}));

    std::promise<StartSummary> promise;
    auto future = promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return start_client_attempt(&endpoint, &client, &promise); });

    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    const StartSummary summary = future.get();
    EXPECT_EQ(summary.error, fiber::common::IoErr::None);
    EXPECT_EQ(summary.state, fiber::quic::QuicConnectionState::Handshaking);
    EXPECT_EQ(summary.endpoint_connections, 1U);
    EXPECT_TRUE(summary.tls_initialized);
    EXPECT_TRUE(summary.initial_keys_ready);
    EXPECT_TRUE(summary.cid_registered);
    EXPECT_TRUE(summary.initial_output_queued);

    group.stop();
    group.join();
}

TEST(QuicClientTest, HandshakeTimeoutCancelsAndDetachesConnection) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicUdpEndpoint endpoint;
    fiber::quic::QuicUdpEndpoint::EndpointOptions endpoint_options{};
    endpoint_options.bind_addr = {fiber::net::IpAddress::loopback_v4(), 0};
    ASSERT_TRUE(endpoint.init(group.at(0), endpoint_options));

    auto tls_context = create_tls_context();
    ASSERT_TRUE(tls_context);
    auto tls_options = make_quic_client_tls(tls_context->get(), false);

    fiber::quic::QuicClient client;
    ASSERT_TRUE(
            client.init(endpoint, tls_options, {.connection_owner = nullptr, .create_connection = create_connection}));

    std::promise<TimeoutSummary> promise;
    auto future = promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return timeout_client_attempt(&endpoint, &client, &promise); });

    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    const TimeoutSummary summary = future.get();
    EXPECT_EQ(summary.error, fiber::common::IoErr::TimedOut);
    EXPECT_EQ(summary.phase, fiber::quic::QuicConnectPhase::Timeout);
    EXPECT_EQ(summary.endpoint_connections, 0U);

    group.stop();
    group.join();
}

TEST(QuicClientTest, CompletesVerifiedLoopbackHandshake) {
    fiber::test::QuicTestTlsFile cert("cert", fiber::test::kQuicTestCertificatePem);
    fiber::test::QuicTestTlsFile key("key", fiber::test::kQuicTestPrivateKeyPem);
    ASSERT_TRUE(cert.valid());
    ASSERT_TRUE(key.valid());

    auto server_context = create_tls_context(cert.path(), key.path());
    ASSERT_TRUE(server_context);
    auto server_tls = make_quic_server_tls(server_context->get());
    auto client_context = create_tls_context({}, {}, cert.path());
    ASSERT_TRUE(client_context);
    auto client_tls = make_quic_client_tls(client_context->get());

    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicUdpEndpoint server_endpoint;
    fiber::quic::QuicUdpEndpoint::Options server_options{};
    server_options.bind_addr = {fiber::net::IpAddress::loopback_v4(), 0};
    server_options.tls = &server_tls;
    server_options.create_connection = create_connection;
    ASSERT_TRUE(server_endpoint.init(group.at(0), server_options));

    fiber::quic::QuicUdpEndpoint client_endpoint;
    fiber::quic::QuicUdpEndpoint::EndpointOptions client_options{};
    client_options.bind_addr = {fiber::net::IpAddress::loopback_v4(), 0};
    ASSERT_TRUE(client_endpoint.init(group.at(0), client_options));

    fiber::quic::QuicClient client;
    ASSERT_TRUE(client.init(client_endpoint, client_tls,
                            {.connection_owner = nullptr, .create_connection = create_connection}));

    std::promise<ConnectSummary> promise;
    auto future = promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return connect_loopback(&server_endpoint, &client_endpoint, &client, "localhost", &promise);
    });

    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    const ConnectSummary summary = future.get();
    EXPECT_EQ(summary.error, fiber::common::IoErr::None);
    EXPECT_EQ(summary.state, fiber::quic::QuicConnectionState::Established);
    EXPECT_EQ(summary.selected_alpn, "fiber-quic-test");
    EXPECT_TRUE(summary.peer_transport_received);
    EXPECT_TRUE(summary.server_scid_adopted);
    EXPECT_FALSE(summary.retry_processed);

    group.stop();
    group.join();
}

TEST_P(QuicClientMtlsTest, EnforcesClientCertificateAuthentication) {
    const QuicMtlsCase &test_case = GetParam();
    fiber::test::QuicTestTlsFile server_cert("server_cert", fiber::test::kQuicTestCertificatePem);
    fiber::test::QuicTestTlsFile server_key("server_key", fiber::test::kQuicTestPrivateKeyPem);
    fiber::test::QuicTestTlsFile client_root("client_root", fiber::test::kRootCertPem);
    const std::string client_chain_contents =
            std::string(fiber::test::kClientCertPem) + fiber::test::kIntermediateCertPem;
    fiber::test::QuicTestTlsFile client_chain("client_chain", client_chain_contents);
    fiber::test::QuicTestTlsFile client_key("client_key", fiber::test::kClientKeyPem);
    ASSERT_TRUE(server_cert.valid());
    ASSERT_TRUE(server_key.valid());
    ASSERT_TRUE(client_root.valid());
    ASSERT_TRUE(client_chain.valid());
    ASSERT_TRUE(client_key.valid());

    const std::string &server_trust =
            test_case.identity == QuicClientIdentityMode::UnknownCa ? server_cert.path() : client_root.path();
    auto server_context = create_tls_context(server_cert.path(), server_key.path(), server_trust);
    ASSERT_TRUE(server_context);
    auto server_tls = make_quic_server_tls(server_context->get(), fiber::net::TlsClientCertificateMode::Required);

    auto client_context = test_case.identity == QuicClientIdentityMode::Anonymous
                                  ? create_tls_context({}, {}, server_cert.path())
                                  : create_tls_context(client_chain.path(), client_key.path(), server_cert.path());
    ASSERT_TRUE(client_context);
    auto client_tls = make_quic_client_tls(client_context->get());

    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicUdpEndpoint server_endpoint;
    fiber::quic::QuicUdpEndpoint::Options server_options{};
    server_options.bind_addr = {fiber::net::IpAddress::loopback_v4(), 0};
    server_options.tls = &server_tls;
    server_options.create_connection = create_connection;
    ASSERT_TRUE(server_endpoint.init(group.at(0), server_options));

    fiber::quic::QuicUdpEndpoint client_endpoint;
    fiber::quic::QuicUdpEndpoint::EndpointOptions client_options{};
    client_options.bind_addr = {fiber::net::IpAddress::loopback_v4(), 0};
    ASSERT_TRUE(client_endpoint.init(group.at(0), client_options));

    fiber::quic::QuicClient client;
    ASSERT_TRUE(client.init(client_endpoint, client_tls,
                            {.connection_owner = nullptr, .create_connection = create_connection}));

    std::promise<ConnectSummary> promise;
    auto future = promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return connect_loopback_confirmed(&server_endpoint, &client_endpoint, &client, "localhost", &promise);
    });

    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    const ConnectSummary summary = future.get();
    if (test_case.expect_success) {
        EXPECT_EQ(summary.error, fiber::common::IoErr::None);
        EXPECT_EQ(summary.state, fiber::quic::QuicConnectionState::Established);
        EXPECT_EQ(summary.selected_alpn, "fiber-quic-test");
    } else {
        EXPECT_NE(summary.error, fiber::common::IoErr::None);
        EXPECT_TRUE(summary.phase == fiber::quic::QuicConnectPhase::Tls ||
                    summary.phase == fiber::quic::QuicConnectPhase::PeerClose);
        EXPECT_NE(summary.tls_alert, 0U);
    }
    EXPECT_EQ(summary.client_endpoint_connections, 0U);
    EXPECT_EQ(summary.server_endpoint_connections, 0U);

    group.stop();
    group.join();
}

INSTANTIATE_TEST_SUITE_P(ClientIdentity, QuicClientMtlsTest,
                         ::testing::Values(QuicMtlsCase{"Trusted", QuicClientIdentityMode::Trusted, true},
                                           QuicMtlsCase{"Anonymous", QuicClientIdentityMode::Anonymous, false},
                                           QuicMtlsCase{"UnknownCa", QuicClientIdentityMode::UnknownCa, false}),
                         [](const ::testing::TestParamInfo<QuicMtlsCase> &info) { return info.param.name; });

TEST(QuicClientTest, RejectsCertificateForWrongHostname) {
    fiber::test::QuicTestTlsFile cert("cert", fiber::test::kQuicTestCertificatePem);
    fiber::test::QuicTestTlsFile key("key", fiber::test::kQuicTestPrivateKeyPem);
    ASSERT_TRUE(cert.valid());
    ASSERT_TRUE(key.valid());

    auto server_context = create_tls_context(cert.path(), key.path());
    ASSERT_TRUE(server_context);
    auto server_tls = make_quic_server_tls(server_context->get());
    auto client_context = create_tls_context({}, {}, cert.path());
    ASSERT_TRUE(client_context);
    auto client_tls = make_quic_client_tls(client_context->get());

    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicUdpEndpoint server_endpoint;
    fiber::quic::QuicUdpEndpoint::Options server_options{};
    server_options.bind_addr = {fiber::net::IpAddress::loopback_v4(), 0};
    server_options.tls = &server_tls;
    server_options.create_connection = create_connection;
    ASSERT_TRUE(server_endpoint.init(group.at(0), server_options));

    fiber::quic::QuicUdpEndpoint client_endpoint;
    fiber::quic::QuicUdpEndpoint::EndpointOptions client_options{};
    client_options.bind_addr = {fiber::net::IpAddress::loopback_v4(), 0};
    ASSERT_TRUE(client_endpoint.init(group.at(0), client_options));

    fiber::quic::QuicClient client;
    ASSERT_TRUE(client.init(client_endpoint, client_tls,
                            {.connection_owner = nullptr, .create_connection = create_connection}));

    std::promise<ConnectSummary> promise;
    auto future = promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return connect_loopback(&server_endpoint, &client_endpoint, &client, "wrong.example", &promise);
    });

    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    const ConnectSummary summary = future.get();
    EXPECT_NE(summary.error, fiber::common::IoErr::None);
    EXPECT_EQ(summary.phase, fiber::quic::QuicConnectPhase::Tls);
    EXPECT_NE(summary.tls_verify_result, 0);

    group.stop();
    group.join();
}

TEST(QuicClientTest, UnknownDcidStatelessResetUsesEndpointTokenIndex) {
    fiber::test::QuicTestTlsFile cert("cert", fiber::test::kQuicTestCertificatePem);
    fiber::test::QuicTestTlsFile key("key", fiber::test::kQuicTestPrivateKeyPem);
    ASSERT_TRUE(cert.valid());
    ASSERT_TRUE(key.valid());

    auto server_context = create_tls_context(cert.path(), key.path());
    ASSERT_TRUE(server_context);
    auto server_tls = make_quic_server_tls(server_context->get());
    auto client_context = create_tls_context({}, {}, cert.path());
    ASSERT_TRUE(client_context);
    auto client_tls = make_quic_client_tls(client_context->get());

    fiber::event::EventLoopGroup group(1);
    group.start();

    std::array<std::uint8_t, fiber::quic::kQuicStatelessResetSecretLength> reset_secret{};
    for (std::size_t i = 0; i < reset_secret.size(); ++i) {
        reset_secret[i] = static_cast<std::uint8_t>(0x40U + i);
    }

    fiber::quic::QuicUdpEndpoint server_endpoint;
    fiber::quic::QuicUdpEndpoint::EndpointOptions server_endpoint_options{};
    server_endpoint_options.bind_addr = {fiber::net::IpAddress::loopback_v4(), 0};
    server_endpoint_options.stateless_reset_secret_set = true;
    server_endpoint_options.stateless_reset_secret = reset_secret;
    fiber::quic::QuicUdpEndpoint::ServerAdmissionOptions server_options{};
    server_options.tls = &server_tls;
    server_options.create_connection = create_connection;
    ASSERT_TRUE(server_endpoint.init(group.at(0), server_endpoint_options, server_options));

    fiber::quic::QuicUdpEndpoint client_endpoint;
    fiber::quic::QuicUdpEndpoint::EndpointOptions client_options{};
    client_options.bind_addr = {fiber::net::IpAddress::loopback_v4(), 0};
    ASSERT_TRUE(client_endpoint.init(group.at(0), client_options));

    fiber::quic::QuicClient client;
    ASSERT_TRUE(client.init(client_endpoint, client_tls,
                            {.connection_owner = nullptr, .create_connection = create_connection}));

    std::promise<StatelessResetSummary> promise;
    auto future = promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return receive_unknown_dcid_stateless_reset(&server_endpoint, &client_endpoint, &client, &reset_secret,
                                                    &promise);
    });

    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    const StatelessResetSummary summary = future.get();
    EXPECT_EQ(summary.error, fiber::common::IoErr::None);
    EXPECT_TRUE(summary.token_installed);
    EXPECT_TRUE(summary.state == fiber::quic::QuicConnectionState::Draining ||
                summary.state == fiber::quic::QuicConnectionState::Closed);
    EXPECT_EQ(summary.close_source, fiber::quic::QuicCloseSource::StatelessReset);

    group.stop();
    group.join();
}

TEST(QuicClientTest, CompletesVerifiedLoopbackHandshakeAfterRetry) {
    fiber::test::QuicTestTlsFile cert("cert", fiber::test::kQuicTestCertificatePem);
    fiber::test::QuicTestTlsFile key("key", fiber::test::kQuicTestPrivateKeyPem);
    ASSERT_TRUE(cert.valid());
    ASSERT_TRUE(key.valid());

    auto server_context = create_tls_context(cert.path(), key.path());
    ASSERT_TRUE(server_context);
    auto server_tls = make_quic_server_tls(server_context->get());
    auto client_context = create_tls_context({}, {}, cert.path());
    ASSERT_TRUE(client_context);
    auto client_tls = make_quic_client_tls(client_context->get());

    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicUdpEndpoint server_endpoint;
    fiber::quic::QuicUdpEndpoint::Options server_options{};
    server_options.bind_addr = {fiber::net::IpAddress::loopback_v4(), 0};
    server_options.tls = &server_tls;
    server_options.create_connection = create_connection;
    server_options.retry = true;
    ASSERT_TRUE(server_endpoint.init(group.at(0), server_options));

    fiber::quic::QuicUdpEndpoint client_endpoint;
    fiber::quic::QuicUdpEndpoint::EndpointOptions client_options{};
    client_options.bind_addr = {fiber::net::IpAddress::loopback_v4(), 0};
    ASSERT_TRUE(client_endpoint.init(group.at(0), client_options));

    fiber::quic::QuicClient client;
    ASSERT_TRUE(client.init(client_endpoint, client_tls,
                            {.connection_owner = nullptr, .create_connection = create_connection}));

    std::promise<ConnectSummary> promise;
    auto future = promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return connect_loopback(&server_endpoint, &client_endpoint, &client, "localhost", &promise);
    });

    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    const ConnectSummary summary = future.get();
    EXPECT_EQ(summary.error, fiber::common::IoErr::None);
    EXPECT_EQ(summary.state, fiber::quic::QuicConnectionState::Established);
    EXPECT_EQ(summary.selected_alpn, "fiber-quic-test");
    EXPECT_TRUE(summary.peer_transport_received);
    EXPECT_TRUE(summary.server_scid_adopted);
    EXPECT_TRUE(summary.retry_processed);

    group.stop();
    group.join();
}

TEST(QuicClientTest, ReusesSessionAndNewTokenWithEarlyData) {
    fiber::test::QuicTestTlsFile cert("cert", fiber::test::kQuicTestCertificatePem);
    fiber::test::QuicTestTlsFile key("key", fiber::test::kQuicTestPrivateKeyPem);
    ASSERT_TRUE(cert.valid());
    ASSERT_TRUE(key.valid());

    auto server_context = create_tls_context(cert.path(), key.path());
    ASSERT_TRUE(server_context);
    auto server_tls = make_quic_server_tls(server_context->get());
    auto client_context = create_tls_context({}, {}, cert.path());
    ASSERT_TRUE(client_context);
    auto client_tls = make_quic_client_tls(client_context->get());

    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicUdpEndpoint server_endpoint;
    fiber::quic::QuicUdpEndpoint::Options server_options{};
    server_options.bind_addr = {fiber::net::IpAddress::loopback_v4(), 0};
    server_options.tls = &server_tls;
    server_options.create_connection = create_server_connection;
    server_options.issue_new_token = true;
    server_options.enable_early_data = true;
    ASSERT_TRUE(server_endpoint.init(group.at(0), server_options));

    fiber::quic::QuicUdpEndpoint client_endpoint;
    fiber::quic::QuicUdpEndpoint::EndpointOptions client_options{};
    client_options.bind_addr = {fiber::net::IpAddress::loopback_v4(), 0};
    ASSERT_TRUE(client_endpoint.init(group.at(0), client_options));

    TestClientCache cache{};
    fiber::quic::QuicClient client;
    fiber::quic::QuicClient::Options client_options_config{};
    client_options_config.create_connection = create_connection;
    client_options_config.cache = {
            .owner = &cache,
            .load = TestClientCache::load,
            .store_session = TestClientCache::store_session,
            .store_token = TestClientCache::store_token,
    };
    ASSERT_TRUE(client.init(client_endpoint, client_tls, client_options_config));

    std::promise<ResumptionSummary> promise;
    auto future = promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return connect_twice_with_cache(&server_endpoint, &client_endpoint, &client, &cache, &promise);
    });

    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    const ResumptionSummary summary = future.get();
    EXPECT_EQ(summary.error, fiber::common::IoErr::None);
    EXPECT_TRUE(summary.session_cached);
    EXPECT_TRUE(summary.token_cached);
    EXPECT_TRUE(summary.session_reused);
    EXPECT_TRUE(summary.token_reused);
    EXPECT_TRUE(summary.early_data_attempted);
    EXPECT_TRUE(summary.cached_session_early_capable);
    EXPECT_TRUE(summary.early_write_ready);
    EXPECT_TRUE(summary.early_stream_queued) << static_cast<int>(summary.early_attach_error);
    EXPECT_TRUE(summary.early_data_accepted);

    group.stop();
    group.join();
}
