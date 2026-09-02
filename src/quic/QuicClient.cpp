#include <fiber/quic/QuicClient.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include <fiber/common/Assert.h>
#include <fiber/net/TlsParams.h>
#include <fiber/quic/QuicProtocol.h>
#include <fiber/quic/QuicUdpEndpoint.h>
#include "quic/QuicCrypto.h"

namespace fiber::quic {

namespace {

[[nodiscard]] bool connection_id_equal(const QuicConnectionId &left, const QuicConnectionId &right) noexcept {
    return left.size() == right.size() && (left.empty() || std::memcmp(left.data(), right.data(), left.size()) == 0);
}

} // namespace

QuicClientCacheKey QuicClient::cache_key(std::string_view server_name,
                                         const net::SocketAddress &remote_addr) const noexcept {
    return QuicClientCacheKey{
            .server_name = server_name,
            .remote_addr = remote_addr,
            .credential = tls_options_ ? tls_options_->credential : nullptr,
            .trust_store = tls_options_ ? tls_options_->trust_store : nullptr,
    };
}

bool QuicClient::store_session(void *owner, QuicConnection &connection, SSL_SESSION *session) noexcept {
    auto *client = static_cast<QuicClient *>(owner);
    if (client == nullptr || session == nullptr || client->options_.cache.store_session == nullptr) {
        return false;
    }
    const QuicClientCacheKey key =
            client->cache_key(connection.client_server_name(), connection.client_cache_remote_addr());
    return client->options_.cache.store_session(client->options_.cache.owner, key, session,
                                                connection.peer_transport().params);
}

void QuicClient::store_token(void *owner, QuicConnection &connection, const std::uint8_t *token,
                             std::size_t token_len) noexcept {
    auto *client = static_cast<QuicClient *>(owner);
    if (client == nullptr || client->options_.cache.store_token == nullptr) {
        return;
    }
    const QuicClientCacheKey key =
            client->cache_key(connection.client_server_name(), connection.client_cache_remote_addr());
    client->options_.cache.store_token(client->options_.cache.owner, key, token, token_len);
}

QuicClientAttempt::QuicClientAttempt(QuicClientAttempt &&other) noexcept :
    connection_(std::move(other.connection_)), handshake_deadline_(other.handshake_deadline_) {
    other.handshake_deadline_ = std::chrono::steady_clock::time_point::max();
}

QuicClientAttempt &QuicClientAttempt::operator=(QuicClientAttempt &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    cancel();
    connection_ = std::move(other.connection_);
    handshake_deadline_ = other.handshake_deadline_;
    other.handshake_deadline_ = std::chrono::steady_clock::time_point::max();
    return *this;
}

QuicClientAttempt::~QuicClientAttempt() { cancel(); }

QuicConnectError QuicClientAttempt::make_error(common::IoErr error) const noexcept {
    QuicConnectError result{};
    result.io_error = error;
    result.phase = error == common::IoErr::TimedOut ? QuicConnectPhase::Timeout : QuicConnectPhase::Handshake;
    if (connection_) {
        result.close = connection_->close_info();
        result.tls_verify_result = connection_->tls().peer_verify_result();
        if (const auto alert = connection_->tls().last_alert()) {
            result.tls_alert = *alert;
        } else if (result.close.error_code >= kQuicCryptoErrorBase &&
                   result.close.error_code <= kQuicCryptoErrorBase + UINT8_MAX) {
            result.tls_alert = static_cast<std::uint8_t>(result.close.error_code - kQuicCryptoErrorBase);
        }
        if (error != common::IoErr::TimedOut) {
            if (connection_->connect_failure() == common::IoErr::NotSupported &&
                !connection_->has_authenticated_server_packet()) {
                result.phase = QuicConnectPhase::VersionNegotiation;
                result.offered_version = kQuicVersion1;
            } else if (result.close.error_code == static_cast<std::uint64_t>(QuicErrorCode::TransportParameterError)) {
                result.phase = QuicConnectPhase::TransportParameters;
            } else if (result.tls_alert != 0 || result.tls_verify_result != 0) {
                result.phase = QuicConnectPhase::Tls;
            }
        }
        if (connection_->close_source() == QuicCloseSource::PeerConnectionClose ||
            connection_->close_source() == QuicCloseSource::StatelessReset) {
            result.phase = QuicConnectPhase::PeerClose;
        }
    }
    return result;
}

async::Task<QuicConnectWaitResult> QuicClientAttempt::wait_connected(std::chrono::milliseconds timeout) noexcept {
    if (!connection_) {
        co_return std::unexpected(make_error(common::IoErr::Canceled));
    }
    const auto now = event::EventLoop::current().now();
    if (handshake_deadline_ != std::chrono::steady_clock::time_point::max()) {
        const auto remaining =
                handshake_deadline_ > now
                        ? std::chrono::duration_cast<std::chrono::milliseconds>(handshake_deadline_ - now)
                        : std::chrono::milliseconds::zero();
        timeout = timeout == std::chrono::milliseconds::max() ? remaining : std::min(timeout, remaining);
    }
    auto waited = co_await connection_->wait_established(timeout);
    if (!waited) {
        co_return std::unexpected(make_error(waited.error()));
    }
    co_return QuicConnectWaitResult{};
}

async::Task<QuicConnectWaitResult> QuicClientAttempt::wait_confirmed(std::chrono::milliseconds timeout) noexcept {
    if (!connection_) {
        co_return std::unexpected(make_error(common::IoErr::Canceled));
    }
    auto waited = co_await connection_->wait_confirmed(timeout);
    if (!waited) {
        co_return std::unexpected(make_error(waited.error()));
    }
    co_return QuicConnectWaitResult{};
}

void QuicClientAttempt::cancel() noexcept {
    QuicConnection *connection = connection_.get();
    if (connection == nullptr) {
        return;
    }
    FIBER_ASSERT(connection->loop() != nullptr);
    FIBER_ASSERT(connection->loop()->in_loop());
    if (!connection->terminal_closing()) {
        connection->close_immediately(QuicErrorCode::NoError);
    }
    connection_.reset();
    handshake_deadline_ = std::chrono::steady_clock::time_point::max();
}

common::IoResult<void> QuicClient::init(QuicUdpEndpoint &endpoint, const net::TlsClientParam &tls_options,
                                        const Options &options) noexcept {
    if (endpoint_ != nullptr || !endpoint.valid() || endpoint.loop_ == nullptr || !tls_options.enabled() ||
        tls_options.alpn.empty() || options.create_connection == nullptr) {
        return std::unexpected(common::IoErr::Invalid);
    }
    endpoint_ = &endpoint;
    tls_options_ = &tls_options;
    options_ = options;
    return {};
}

QuicConnectError QuicClient::error(QuicConnectPhase phase, common::IoErr io_error) noexcept {
    return QuicConnectError{.phase = phase, .io_error = io_error, .offered_version = kQuicVersion1};
}

std::expected<QuicClientAttempt, QuicConnectError>
QuicClient::start_connect(const QuicClientConnectOptions &options) noexcept {
    if (endpoint_ == nullptr || tls_options_ == nullptr || !endpoint_->running() || endpoint_->loop_ == nullptr ||
        !endpoint_->loop_->in_loop() || options.remote_addr.port() == 0 || options.remote_addr.ip().is_unspecified() ||
        options.handshake_timeout < std::chrono::milliseconds::zero()) {
        return std::unexpected(error(QuicConnectPhase::Endpoint, common::IoErr::Invalid));
    }

    QuicClientCachedState cached{};
    if (options_.cache.load != nullptr) {
        if (!options_.cache.load(options_.cache.owner, cache_key(options.server_name, options.remote_addr), cached)) {
            cached = {};
        }
        if ((cached.token == nullptr && cached.token_len != 0) || cached.token_len > 16 * 1024) {
            return std::unexpected(error(QuicConnectPhase::Connection, common::IoErr::Invalid));
        }
    }

    auto original_dcid = endpoint_->generate_connection_id();
    if (!original_dcid) {
        return std::unexpected(error(QuicConnectPhase::Connection, original_dcid.error()));
    }

    common::IoResult<QuicConnectionId> local_cid = std::unexpected(common::IoErr::Already);
    for (std::uint8_t attempt = 0; attempt < 8; ++attempt) {
        local_cid = endpoint_->generate_unique_connection_id();
        if (!local_cid) {
            break;
        }
        if (!connection_id_equal(*local_cid, *original_dcid)) {
            break;
        }
        local_cid = std::unexpected(common::IoErr::Already);
    }
    if (!local_cid) {
        return std::unexpected(error(QuicConnectPhase::Connection, local_cid.error()));
    }

    QuicConnection::Options connection_options{};
    connection_options.role = QuicConnectionRole::Client;
    connection_options.local_addr = endpoint_->local_addr();
    connection_options.remote_addr = options.remote_addr;
    connection_options.original_destination_connection_id = *original_dcid;
    connection_options.initial_destination_connection_id = *original_dcid;
    connection_options.local_connection_id = *local_cid;
    connection_options.remote_connection_id = *original_dcid;
    connection_options.transport = options.transport;
    connection_options.keepalive_interval = options.keepalive_interval;
    connection_options.recv_flow = options.recv_flow;
    connection_options.max_peer_bidirectional_streams = options.max_peer_bidirectional_streams;
    connection_options.max_peer_unidirectional_streams = options.max_peer_unidirectional_streams;
    const bool attempt_early_data =
            options.enable_early_data && cached.session != nullptr && cached.has_remembered_transport;
    connection_options.max_local_bidirectional_streams =
            attempt_early_data ? cached.remembered_transport.initial_max_streams_bidi : 0;
    connection_options.max_local_unidirectional_streams =
            attempt_early_data ? cached.remembered_transport.initial_max_streams_uni : 0;
    connection_options.output_frame_pool = &endpoint_->output_frame_pool_;
    connection_options.crypto_block_pool = &endpoint_->crypto_block_pool_;
    connection_options.recv_storage_parent = &endpoint_->recv_storage_budget_;
    connection_options.loop = endpoint_->loop_;
    connection_options.owner = options.application_owner;
    connection_options.ops = options.application_ops;
    connection_options.enable_early_data = options.enable_early_data;
    connection_options.remembered_peer_transport = cached.remembered_transport;
    connection_options.has_remembered_peer_transport = attempt_early_data;
    connection_options.client_server_name = options.server_name;
    connection_options.client_cache_remote_addr = options.remote_addr;
    connection_options.client_tls_credential = tls_options_->credential;
    connection_options.client_trust_store = tls_options_->trust_store;
    connection_options.client_cache_owner = this;
    connection_options.on_new_tls_session = options_.cache.store_session != nullptr ? store_session : nullptr;
    connection_options.on_new_token = options_.cache.store_token != nullptr ? store_token : nullptr;

    QuicConnection::Lease endpoint_lease = options_.create_connection(options_.connection_owner, connection_options);
    QuicConnection *connection = endpoint_lease.get();
    if (connection == nullptr) {
        return std::unexpected(error(QuicConnectPhase::Connection, common::IoErr::NoMem));
    }
    if (connection->role() != QuicConnectionRole::Client || connection->on_destroy_ == nullptr) {
        return std::unexpected(error(QuicConnectPhase::Connection, common::IoErr::Invalid));
    }

    auto token = connection->set_initial_token(cached.token, cached.token_len);
    if (!token) {
        return std::unexpected(error(QuicConnectPhase::Connection, token.error()));
    }

    auto initialized_crypto = connection->init_initial_crypto(*original_dcid);
    if (!initialized_crypto) {
        return std::unexpected(error(QuicConnectPhase::InitialCrypto, initialized_crypto.error()));
    }
    auto initialized_tls = connection->tls().init_client(*tls_options_, *connection, options.server_name.c_str(),
                                                         options.allow_insecure, cached.session);
    if (!initialized_tls) {
        return std::unexpected(error(QuicConnectPhase::Tls, initialized_tls.error()));
    }
    auto driven = connection->tls().drive_handshake();
    if (!driven && driven.error() != common::IoErr::WouldBlock) {
        return std::unexpected(error(QuicConnectPhase::Tls, driven.error()));
    }
    auto started = connection->start_handshake();
    if (!started) {
        return std::unexpected(error(QuicConnectPhase::Handshake, started.error()));
    }

    QuicConnection::Lease caller_lease = connection->lease();
    auto attached = endpoint_->attach_client_connection(std::move(endpoint_lease));
    if (!attached) {
        caller_lease.reset();
        return std::unexpected(error(QuicConnectPhase::Endpoint, attached.error()));
    }
    endpoint_->schedule_send(*connection);
    const auto handshake_deadline = options.handshake_timeout == std::chrono::milliseconds::max()
                                            ? std::chrono::steady_clock::time_point::max()
                                            : endpoint_->loop_->now() + options.handshake_timeout;
    return QuicClientAttempt(std::move(caller_lease), handshake_deadline);
}

async::Task<QuicConnectResult> QuicClient::connect(const QuicClientConnectOptions &options) noexcept {
    auto started = start_connect(options);
    if (!started) {
        co_return std::unexpected(started.error());
    }
    QuicClientAttempt attempt = std::move(*started);
    auto connected = co_await attempt.wait_connected();
    if (!connected) {
        co_return std::unexpected(connected.error());
    }
    co_return attempt.release();
}

} // namespace fiber::quic
