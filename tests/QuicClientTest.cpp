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
#include <fiber/net/TlsCredential.h>
#include <fiber/net/TlsServerHandshakeConfig.h>
#include <fiber/net/TrustStore.h>
#include <fiber/net/UdpSocket.h>
#include <fiber/quic/QuicClientConnect.h>
#include <fiber/quic/QuicUdpEndpoint.h>
#include "QuicTestTlsCertificate.h"
#include "TlsClientIdentityTestData.h"

namespace {

using namespace std::chrono_literals;

constexpr std::string_view kQuicTestAlpn[] = {"fiber-quic-test"};

void destroy_connection(void *, fiber::quic::QuicConnection &connection) noexcept { delete &connection; }

void destroy_stream(void *, fiber::quic::QuicStream &stream) noexcept { delete &stream; }

fiber::quic::QuicStream::Lease create_stream(void *, std::uint64_t) noexcept {
    return fiber::quic::QuicStream::Lease::adopt(new (std::nothrow) fiber::quic::QuicStream(nullptr, destroy_stream));
}

fiber::quic::QuicConnection::Lease
create_server_connection(void *owner, fiber::quic::QuicUdpEndpoint &endpoint,
                         const fiber::quic::QuicConnection::Options &options) noexcept {
    fiber::quic::QuicConnection::Options owned = options;
    owned.on_destroy = destroy_connection;
    owned.owner = owner;
    owned.ops.create_stream = create_stream;
    return fiber::quic::QuicConnection::Lease::adopt(new (std::nothrow) fiber::quic::QuicConnection(endpoint, owned));
}

fiber::quic::QuicConnection::Lease create_connection(void *, fiber::quic::QuicUdpEndpoint &endpoint,
                                                     const fiber::quic::QuicConnection::Options &options) noexcept {
    fiber::quic::QuicConnection::Options owned = options;
    owned.on_destroy = destroy_connection;
    return fiber::quic::QuicConnection::Lease::adopt(new (std::nothrow) fiber::quic::QuicConnection(endpoint, owned));
}

// Options for a caller-owned client connection (no on_destroy): the endpoint
// hosts it only until it detaches, and the coroutine frame that builds it
// destroys it after wait_closed().
fiber::common::IoResult<fiber::quic::QuicConnection::Options>
make_client_options(fiber::quic::QuicUdpEndpoint &endpoint, const fiber::net::SocketAddress &remote_addr) noexcept {
    auto identity = endpoint.allocate_client_identity();
    if (!identity) {
        return std::unexpected(identity.error());
    }
    fiber::quic::QuicConnection::Options options{};
    options.role = fiber::quic::QuicConnectionRole::Client;
    options.local_addr = endpoint.local_addr();
    options.remote_addr = remote_addr;
    options.original_destination_connection_id = identity->original_destination_connection_id;
    options.initial_destination_connection_id = identity->original_destination_connection_id;
    options.remote_connection_id = identity->original_destination_connection_id;
    options.local_connection_id = identity->local_connection_id;
    options.max_local_bidirectional_streams = 0;
    options.max_local_unidirectional_streams = 0;
    options.ops.create_stream = create_stream;
    return options;
}

fiber::quic::QuicClientConnectParams make_connect_params(const fiber::net::TlsClientSecurity &security,
                                                         std::string_view server_name,
                                                         std::string_view verify_name = {},
                                                         bool allow_insecure = false) noexcept {
    fiber::quic::QuicClientConnectParams params{};
    params.tls.security = security;
    params.tls.min_version = 0x0304;
    params.tls.max_version = 0x0304;
    params.tls.alpn = kQuicTestAlpn;
    params.tls.server_name = server_name;
    params.tls.verify_name = verify_name;
    params.allow_insecure = allow_insecure;
    return params;
}

// Tears a caller-owned connection down far enough to destroy it: an immediate
// close if it is still open, then the detach wait.
fiber::async::Task<void> close_and_wait(fiber::quic::QuicConnection &connection) noexcept {
    if (!connection.terminal_closing()) {
        connection.close_immediately();
    }
    co_await connection.wait_closed();
}

struct QuicTestTls {
    std::unique_ptr<fiber::net::TlsCredential> credential;
    std::unique_ptr<fiber::net::TrustStore> trust_store;
};

fiber::common::IoResult<QuicTestTls> create_quic_tls(std::string_view certificate = {},
                                                     std::string_view private_key = {}, std::string_view trust = {}) {
    QuicTestTls material{};
    if (!certificate.empty()) {
        fiber::net::TlsCredentialOptions credential_options{};
        credential_options.certificate_chain = fiber::net::TlsPemSource::from_file(std::string(certificate));
        credential_options.private_key = fiber::net::TlsPemSource::from_file(std::string(private_key));
        auto credential = fiber::net::TlsCredential::create(credential_options);
        if (!credential) {
            return std::unexpected(credential.error());
        }
        material.credential = std::move(*credential);
    }
    if (!trust.empty()) {
        auto trust_store = fiber::net::TrustStore::create(fiber::net::TrustStoreOptions::from_file(std::string(trust)));
        if (!trust_store) {
            return std::unexpected(trust_store.error());
        }
        material.trust_store = std::move(*trust_store);
    }
    return material;
}

fiber::net::TlsClientSecurity make_quic_client_tls(const QuicTestTls &material, bool verify_peer = true) {
    fiber::net::TlsClientSecurity options{};
    options.credential = material.credential.get();
    options.trust_store = material.trust_store.get();
    options.verify_peer = verify_peer;
    return options;
}

fiber::net::TlsServerParam make_quic_server_tls(
        const QuicTestTls &material,
        fiber::net::TlsClientCertificateMode client_certificate_mode = fiber::net::TlsClientCertificateMode::None) {
    fiber::net::TlsServerParam options{};
    options.configure_callback = &fiber::net::configure_tls_with_credential;
    options.configure_ctx = material.credential.get();
    options.trust_store = material.trust_store.get();
    options.client_certificate_mode = client_certificate_mode;
    options.min_version = 0x0304;
    options.max_version = 0x0304;
    options.alpn = kQuicTestAlpn;
    return options;
}

struct StartSummary {
    fiber::common::IoErr error = fiber::common::IoErr::None;
    fiber::common::IoErr missing_alpn_error = fiber::common::IoErr::None;
    fiber::quic::QuicConnectPhase missing_alpn_phase = fiber::quic::QuicConnectPhase::Handshake;
    fiber::quic::QuicConnectionState missing_alpn_state = fiber::quic::QuicConnectionState::Init;
    fiber::quic::QuicConnectionState state = fiber::quic::QuicConnectionState::Closed;
    std::size_t endpoint_connections = 0;
    std::size_t endpoint_connections_after_close = 0;
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

fiber::async::DetachedTask start_client_attempt(fiber::quic::QuicUdpEndpoint *endpoint,
                                                const fiber::net::TlsClientSecurity *security,
                                                std::promise<StartSummary> *promise) {
    StartSummary summary{};
    auto started_endpoint = endpoint->start();
    if (!started_endpoint) {
        summary.error = started_endpoint.error();
        promise->set_value(summary);
        co_return;
    }
    const fiber::net::SocketAddress remote_addr{fiber::net::IpAddress::loopback_v4(), 4433};

    {
        // An empty ALPN list is rejected when the client SSL is created; the
        // connection never attaches and is destructible right away.
        auto options = make_client_options(*endpoint, remote_addr);
        if (!options) {
            summary.error = options.error();
            co_await endpoint->shutdown();
            promise->set_value(summary);
            co_return;
        }
        fiber::quic::QuicConnection connection(*endpoint, *options);
        auto params = make_connect_params(*security, "localhost", {}, true);
        params.tls.alpn = {};
        auto connected = connection.connect(params);
        summary.missing_alpn_error = connected ? fiber::common::IoErr::None : connected.error();
        summary.missing_alpn_phase = connection.connect_error(summary.missing_alpn_error).phase;
        summary.missing_alpn_state = connection.state();
        co_await connection.wait_closed();
    }

    auto options = make_client_options(*endpoint, remote_addr);
    if (!options) {
        summary.error = options.error();
        co_await endpoint->shutdown();
        promise->set_value(summary);
        co_return;
    }
    fiber::quic::QuicConnection connection(*endpoint, *options);
    auto connected = connection.connect(make_connect_params(*security, "localhost", {}, true));
    if (!connected) {
        summary.error = connected.error();
        co_await connection.wait_closed();
        co_await endpoint->shutdown();
        promise->set_value(summary);
        co_return;
    }
    summary.state = connection.state();
    summary.endpoint_connections = endpoint->active_connection_count();
    summary.tls_initialized = connection.tls().initialized();
    summary.initial_keys_ready = connection.crypto().initial_ready();
    summary.cid_registered = endpoint->find_connection(connection.local_connection_id()) == &connection;
    summary.initial_output_queued =
            !connection.packet_number_space(fiber::quic::QuicEncryptionLevel::Initial).pending_frames.empty();

    co_await close_and_wait(connection);
    summary.endpoint_connections_after_close = endpoint->active_connection_count();
    co_await endpoint->shutdown();
    promise->set_value(summary);
}

fiber::async::DetachedTask timeout_client_attempt(fiber::quic::QuicUdpEndpoint *endpoint,
                                                  const fiber::net::TlsClientSecurity *security,
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
        co_await endpoint->shutdown();
        promise->set_value(summary);
        co_return;
    }

    auto options = make_client_options(*endpoint, blackhole.local_addr());
    if (!options) {
        summary.error = options.error();
        blackhole.close();
        co_await endpoint->shutdown();
        promise->set_value(summary);
        co_return;
    }
    fiber::quic::QuicConnection connection(*endpoint, *options);
    auto connected = connection.connect(make_connect_params(*security, "localhost", {}, true));
    if (!connected) {
        summary.error = connected.error();
        summary.phase = connection.connect_error(connected.error()).phase;
    } else {
        auto established = co_await connection.wait_established(10ms);
        if (established) {
            summary.error = fiber::common::IoErr::Already;
        } else {
            summary.error = established.error();
            summary.phase = connection.connect_error(established.error()).phase;
        }
    }
    co_await close_and_wait(connection);
    summary.endpoint_connections = endpoint->active_connection_count();
    blackhole.close();
    co_await endpoint->shutdown();
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

bool cache_store_session(void *owner, fiber::quic::QuicConnection &connection, SSL_SESSION *session) noexcept {
    return TestClientCache::store_session(owner, {}, session, connection.peer_transport().params);
}

void cache_store_token(void *owner, fiber::quic::QuicConnection &, const std::uint8_t *token,
                       std::size_t token_len) noexcept {
    TestClientCache::store_token(owner, {}, token, token_len);
}

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
        const fiber::net::TlsClientSecurity *security,
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

    auto options = make_client_options(*client_endpoint,
                                       {fiber::net::IpAddress::loopback_v4(), server_endpoint->local_addr().port()});
    if (!options) {
        summary.error = options.error();
        co_await client_endpoint->shutdown();
        co_await server_endpoint->shutdown();
        promise->set_value(summary);
        co_return;
    }
    fiber::quic::QuicConnection connection(*client_endpoint, *options);
    auto connected = connection.connect(make_connect_params(*security, "localhost"));
    if (!connected) {
        summary.error = connected.error();
    } else if (auto established = co_await connection.wait_established(2s); !established) {
        summary.error = established.error();
    } else {
        std::array<std::uint8_t, fiber::quic::kStatelessResetTokenLength> token{};
        std::array<std::uint8_t, 64> packet{};
        packet.fill(0x5a);
        packet[0] = fiber::quic::kPacketFlagFixed | 0x03U;
        if (!create_reset_token(*secret, connection.server_initial_source_connection_id(), token.data())) {
            summary.error = fiber::common::IoErr::Invalid;
        } else {
            std::memcpy(packet.data() + packet.size() - token.size(), token.data(), token.size());
            summary.token_installed = connection.detects_stateless_reset(packet.data(), packet.size());

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
                    summary.state = connection.state();
                    summary.close_source = connection.close_source();
                }
                sender.close();
            }
        }
    }

    // The reverse teardown order: shutdown() closes and detaches the
    // caller-owned connection, which this frame destroys afterwards.
    co_await client_endpoint->shutdown();
    co_await server_endpoint->shutdown();
    promise->set_value(summary);
}

fiber::async::DetachedTask connect_twice_with_cache(fiber::quic::QuicUdpEndpoint *server_endpoint,
                                                    fiber::quic::QuicUdpEndpoint *client_endpoint,
                                                    const fiber::net::TlsClientSecurity *security,
                                                    TestClientCache *cache, std::promise<ResumptionSummary> *promise) {
    ResumptionSummary summary{};
    auto server_started = server_endpoint->start();
    auto client_started = client_endpoint->start();
    if (!server_started || !client_started) {
        summary.error = !server_started ? server_started.error() : client_started.error();
        promise->set_value(summary);
        co_return;
    }
    const fiber::net::SocketAddress remote_addr{fiber::net::IpAddress::loopback_v4(),
                                                server_endpoint->local_addr().port()};
    auto make_options = [&]() {
        auto options = make_client_options(*client_endpoint, remote_addr);
        if (options) {
            options->owner = cache;
            options->ops.on_new_tls_session = cache_store_session;
            options->ops.on_new_token = cache_store_token;
        }
        return options;
    };

    {
        auto options = make_options();
        if (!options) {
            summary.error = options.error();
            co_await client_endpoint->shutdown();
            co_await server_endpoint->shutdown();
            promise->set_value(summary);
            co_return;
        }
        fiber::quic::QuicConnection first(*client_endpoint, *options);
        auto connected = first.connect(make_connect_params(*security, "localhost"));
        if (!connected) {
            summary.error = connected.error();
        } else if (auto established = co_await first.wait_established(2s); !established) {
            summary.error = established.error();
        } else {
            (void) co_await first.wait_confirmed(2s);
            co_await fiber::async::sleep(20ms);
            summary.session_cached = cache->session != nullptr;
            summary.token_cached = !cache->token.empty();
        }
        co_await close_and_wait(first);
        co_await fiber::async::sleep(5ms);
    }

    if (summary.error == fiber::common::IoErr::None) {
        fiber::quic::QuicClientCachedState cached{};
        if (!TestClientCache::load(cache, {}, cached)) {
            cached = {};
        }
        summary.cached_session_early_capable =
                cached.session != nullptr && SSL_SESSION_early_data_capable(cached.session) == 1;
        auto options = make_options();
        if (!options) {
            summary.error = options.error();
        } else {
            fiber::quic::QuicConnection second(*client_endpoint, *options);
            auto params = make_connect_params(*security, "localhost");
            params.resumption_session = cached.session;
            params.token = cached.token;
            params.token_len = cached.token_len;
            params.enable_early_data = true;
            params.remembered_peer_transport = cached.has_remembered_transport ? &cached.remembered_transport : nullptr;
            auto connected = second.connect(params);
            if (!connected) {
                summary.error = connected.error();
            } else {
                summary.early_write_ready = second.crypto().early_write().ready();
                auto stream = fiber::quic::QuicStream::Lease::adopt(
                        new (std::nothrow) fiber::quic::QuicStream(nullptr, destroy_stream));
                auto attached =
                        second.try_attach_local_stream(std::move(stream), fiber::quic::QuicStreamType::Bidirectional,
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
                auto established = co_await second.wait_established(2s);
                if (!established) {
                    summary.error = established.error();
                } else {
                    summary.session_reused = second.tls().session_reused();
                    summary.token_reused = second.initial_token().readable() == cache->token.size();
                    summary.early_data_attempted = second.early_data_attempted();
                    summary.early_data_accepted = second.early_data_accepted();
                }
            }
            co_await close_and_wait(second);
        }
    }

    co_await client_endpoint->shutdown();
    co_await server_endpoint->shutdown();
    promise->set_value(summary);
}

void fill_connect_error(ConnectSummary &summary, const fiber::quic::QuicConnectError &error) noexcept {
    summary.error = error.io_error;
    summary.phase = error.phase;
    summary.tls_verify_result = error.tls_verify_result;
    summary.tls_alert = error.tls_alert;
}

void fill_connected(ConnectSummary &summary, const fiber::quic::QuicConnection &connection) {
    summary.state = connection.state();
    summary.selected_alpn.assign(connection.tls().selected_alpn());
    summary.peer_transport_received = connection.peer_transport_params_received();
    summary.server_scid_adopted = connection.has_server_initial_source_connection_id();
    summary.retry_processed = connection.retry_processed();
}

// Connects, waits for Established (or, with `confirmed`, for the handshake to
// be confirmed), records the outcome, tears the connection down and shuts
// both endpoints.
fiber::async::DetachedTask connect_loopback(fiber::quic::QuicUdpEndpoint *server_endpoint,
                                            fiber::quic::QuicUdpEndpoint *client_endpoint,
                                            const fiber::net::TlsClientSecurity *security, const char *server_name,
                                            std::promise<ConnectSummary> *promise, std::string_view verify_name = {},
                                            bool confirmed = false) {
    ConnectSummary summary{};
    auto server_started = server_endpoint->start();
    auto client_started = client_endpoint->start();
    if (!server_started || !client_started) {
        summary.error = !server_started ? server_started.error() : client_started.error();
        co_await client_endpoint->shutdown();
        co_await server_endpoint->shutdown();
        promise->set_value(std::move(summary));
        co_return;
    }

    auto options = make_client_options(*client_endpoint,
                                       {fiber::net::IpAddress::loopback_v4(), server_endpoint->local_addr().port()});
    if (!options) {
        summary.error = options.error();
    } else {
        fiber::quic::QuicConnection connection(*client_endpoint, *options);
        auto connected = connection.connect(make_connect_params(*security, server_name, verify_name));
        if (!connected) {
            fill_connect_error(summary, connection.connect_error(connected.error()));
        } else {
            fiber::common::IoResult<void> waited{};
            if (confirmed) {
                waited = co_await connection.wait_confirmed(2s);
            } else {
                waited = co_await connection.wait_established(2s);
            }
            if (!waited) {
                fill_connect_error(summary, connection.connect_error(waited.error()));
            } else {
                fill_connected(summary, connection);
            }
        }
        co_await close_and_wait(connection);
    }

    co_await client_endpoint->shutdown();
    co_await server_endpoint->shutdown();
    summary.client_endpoint_connections = client_endpoint->active_connection_count();
    summary.server_endpoint_connections = server_endpoint->active_connection_count();
    promise->set_value(std::move(summary));
}

} // namespace

TEST(QuicClientTest, ConnectAttachesAndQueuesClientInitial) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicUdpEndpoint endpoint(group.at(0));
    fiber::quic::QuicUdpEndpoint::EndpointOptions endpoint_options{};
    endpoint_options.bind_addr = {fiber::net::IpAddress::loopback_v4(), 0};
    ASSERT_TRUE(endpoint.init(endpoint_options));

    auto tls_material = create_quic_tls();
    ASSERT_TRUE(tls_material);
    auto tls_options = make_quic_client_tls(*tls_material, false);

    std::promise<StartSummary> promise;
    auto future = promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return start_client_attempt(&endpoint, &tls_options, &promise); });

    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    const StartSummary summary = future.get();
    EXPECT_EQ(summary.missing_alpn_error, fiber::common::IoErr::Invalid);
    EXPECT_EQ(summary.missing_alpn_phase, fiber::quic::QuicConnectPhase::Tls);
    EXPECT_EQ(summary.missing_alpn_state, fiber::quic::QuicConnectionState::Closed);
    EXPECT_EQ(summary.error, fiber::common::IoErr::None);
    EXPECT_EQ(summary.state, fiber::quic::QuicConnectionState::Handshaking);
    EXPECT_EQ(summary.endpoint_connections, 1U);
    EXPECT_EQ(summary.endpoint_connections_after_close, 0U);
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

    fiber::quic::QuicUdpEndpoint endpoint(group.at(0));
    fiber::quic::QuicUdpEndpoint::EndpointOptions endpoint_options{};
    endpoint_options.bind_addr = {fiber::net::IpAddress::loopback_v4(), 0};
    ASSERT_TRUE(endpoint.init(endpoint_options));

    auto tls_material = create_quic_tls();
    ASSERT_TRUE(tls_material);
    auto tls_options = make_quic_client_tls(*tls_material, false);

    std::promise<TimeoutSummary> promise;
    auto future = promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return timeout_client_attempt(&endpoint, &tls_options, &promise); });

    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    const TimeoutSummary summary = future.get();
    EXPECT_EQ(summary.error, fiber::common::IoErr::TimedOut);
    EXPECT_EQ(summary.phase, fiber::quic::QuicConnectPhase::Timeout);
    EXPECT_EQ(summary.endpoint_connections, 0U);

    group.stop();
    group.join();
}

TEST(QuicClientTest, VerifyNameMayDifferFromServerName) {
    fiber::test::QuicTestTlsFile cert("cert", fiber::test::kQuicTestCertificatePem);
    fiber::test::QuicTestTlsFile key("key", fiber::test::kQuicTestPrivateKeyPem);
    ASSERT_TRUE(cert.valid());
    ASSERT_TRUE(key.valid());

    auto server_material = create_quic_tls(cert.path(), key.path());
    ASSERT_TRUE(server_material);
    auto server_tls = make_quic_server_tls(*server_material);
    auto client_material = create_quic_tls({}, {}, cert.path());
    ASSERT_TRUE(client_material);
    auto client_tls = make_quic_client_tls(*client_material);

    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicUdpEndpoint server_endpoint(group.at(0));
    fiber::quic::QuicUdpEndpoint::Options server_options{};
    server_options.bind_addr = {fiber::net::IpAddress::loopback_v4(), 0};
    server_options.tls = &server_tls;
    server_options.create_connection = create_connection;
    ASSERT_TRUE(server_endpoint.init(server_options));

    fiber::quic::QuicUdpEndpoint client_endpoint(group.at(0));
    fiber::quic::QuicUdpEndpoint::EndpointOptions client_options{};
    client_options.bind_addr = {fiber::net::IpAddress::loopback_v4(), 0};
    ASSERT_TRUE(client_endpoint.init(client_options));

    std::promise<ConnectSummary> promise;
    auto future = promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return connect_loopback(&server_endpoint, &client_endpoint, &client_tls, "routing.example", &promise,
                                "localhost");
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
    auto server_material = create_quic_tls(server_cert.path(), server_key.path(), server_trust);
    ASSERT_TRUE(server_material);
    auto server_tls = make_quic_server_tls(*server_material, fiber::net::TlsClientCertificateMode::Required);

    auto client_material = test_case.identity == QuicClientIdentityMode::Anonymous
                                   ? create_quic_tls({}, {}, server_cert.path())
                                   : create_quic_tls(client_chain.path(), client_key.path(), server_cert.path());
    ASSERT_TRUE(client_material);
    auto client_tls = make_quic_client_tls(*client_material);

    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicUdpEndpoint server_endpoint(group.at(0));
    fiber::quic::QuicUdpEndpoint::Options server_options{};
    server_options.bind_addr = {fiber::net::IpAddress::loopback_v4(), 0};
    server_options.tls = &server_tls;
    server_options.create_connection = create_connection;
    ASSERT_TRUE(server_endpoint.init(server_options));

    fiber::quic::QuicUdpEndpoint client_endpoint(group.at(0));
    fiber::quic::QuicUdpEndpoint::EndpointOptions client_options{};
    client_options.bind_addr = {fiber::net::IpAddress::loopback_v4(), 0};
    ASSERT_TRUE(client_endpoint.init(client_options));

    std::promise<ConnectSummary> promise;
    auto future = promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return connect_loopback(&server_endpoint, &client_endpoint, &client_tls, "localhost", &promise, {},
                                /*confirmed=*/true);
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

    auto server_material = create_quic_tls(cert.path(), key.path());
    ASSERT_TRUE(server_material);
    auto server_tls = make_quic_server_tls(*server_material);
    auto client_material = create_quic_tls({}, {}, cert.path());
    ASSERT_TRUE(client_material);
    auto client_tls = make_quic_client_tls(*client_material);

    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicUdpEndpoint server_endpoint(group.at(0));
    fiber::quic::QuicUdpEndpoint::Options server_options{};
    server_options.bind_addr = {fiber::net::IpAddress::loopback_v4(), 0};
    server_options.tls = &server_tls;
    server_options.create_connection = create_connection;
    ASSERT_TRUE(server_endpoint.init(server_options));

    fiber::quic::QuicUdpEndpoint client_endpoint(group.at(0));
    fiber::quic::QuicUdpEndpoint::EndpointOptions client_options{};
    client_options.bind_addr = {fiber::net::IpAddress::loopback_v4(), 0};
    ASSERT_TRUE(client_endpoint.init(client_options));

    std::promise<ConnectSummary> promise;
    auto future = promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return connect_loopback(&server_endpoint, &client_endpoint, &client_tls, "wrong.example", &promise);
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

    auto server_material = create_quic_tls(cert.path(), key.path());
    ASSERT_TRUE(server_material);
    auto server_tls = make_quic_server_tls(*server_material);
    auto client_material = create_quic_tls({}, {}, cert.path());
    ASSERT_TRUE(client_material);
    auto client_tls = make_quic_client_tls(*client_material);

    fiber::event::EventLoopGroup group(1);
    group.start();

    std::array<std::uint8_t, fiber::quic::kQuicStatelessResetSecretLength> reset_secret{};
    for (std::size_t i = 0; i < reset_secret.size(); ++i) {
        reset_secret[i] = static_cast<std::uint8_t>(0x40U + i);
    }

    fiber::quic::QuicUdpEndpoint server_endpoint(group.at(0));
    fiber::quic::QuicUdpEndpoint::EndpointOptions server_endpoint_options{};
    server_endpoint_options.bind_addr = {fiber::net::IpAddress::loopback_v4(), 0};
    server_endpoint_options.stateless_reset_secret_set = true;
    server_endpoint_options.stateless_reset_secret = reset_secret;
    fiber::quic::QuicUdpEndpoint::ServerAdmissionOptions server_options{};
    server_options.tls = &server_tls;
    server_options.create_connection = create_connection;
    ASSERT_TRUE(server_endpoint.init(server_endpoint_options, server_options));

    fiber::quic::QuicUdpEndpoint client_endpoint(group.at(0));
    fiber::quic::QuicUdpEndpoint::EndpointOptions client_options{};
    client_options.bind_addr = {fiber::net::IpAddress::loopback_v4(), 0};
    ASSERT_TRUE(client_endpoint.init(client_options));

    std::promise<StatelessResetSummary> promise;
    auto future = promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return receive_unknown_dcid_stateless_reset(&server_endpoint, &client_endpoint, &client_tls, &reset_secret,
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

    auto server_material = create_quic_tls(cert.path(), key.path());
    ASSERT_TRUE(server_material);
    auto server_tls = make_quic_server_tls(*server_material);
    auto client_material = create_quic_tls({}, {}, cert.path());
    ASSERT_TRUE(client_material);
    auto client_tls = make_quic_client_tls(*client_material);

    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicUdpEndpoint server_endpoint(group.at(0));
    fiber::quic::QuicUdpEndpoint::Options server_options{};
    server_options.bind_addr = {fiber::net::IpAddress::loopback_v4(), 0};
    server_options.tls = &server_tls;
    server_options.create_connection = create_connection;
    server_options.retry = true;
    ASSERT_TRUE(server_endpoint.init(server_options));

    fiber::quic::QuicUdpEndpoint client_endpoint(group.at(0));
    fiber::quic::QuicUdpEndpoint::EndpointOptions client_options{};
    client_options.bind_addr = {fiber::net::IpAddress::loopback_v4(), 0};
    ASSERT_TRUE(client_endpoint.init(client_options));

    std::promise<ConnectSummary> promise;
    auto future = promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return connect_loopback(&server_endpoint, &client_endpoint, &client_tls, "localhost", &promise);
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

    auto server_material = create_quic_tls(cert.path(), key.path());
    ASSERT_TRUE(server_material);
    auto server_tls = make_quic_server_tls(*server_material);
    auto client_material = create_quic_tls({}, {}, cert.path());
    ASSERT_TRUE(client_material);
    auto client_tls = make_quic_client_tls(*client_material);

    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicUdpEndpoint server_endpoint(group.at(0));
    fiber::quic::QuicUdpEndpoint::Options server_options{};
    server_options.bind_addr = {fiber::net::IpAddress::loopback_v4(), 0};
    server_options.tls = &server_tls;
    server_options.create_connection = create_server_connection;
    server_options.issue_new_token = true;
    server_tls.enable_early_data = true;
    ASSERT_TRUE(server_endpoint.init(server_options));

    fiber::quic::QuicUdpEndpoint client_endpoint(group.at(0));
    fiber::quic::QuicUdpEndpoint::EndpointOptions client_options{};
    client_options.bind_addr = {fiber::net::IpAddress::loopback_v4(), 0};
    ASSERT_TRUE(client_endpoint.init(client_options));

    TestClientCache cache{};

    std::promise<ResumptionSummary> promise;
    auto future = promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return connect_twice_with_cache(&server_endpoint, &client_endpoint, &client_tls, &cache, &promise);
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
