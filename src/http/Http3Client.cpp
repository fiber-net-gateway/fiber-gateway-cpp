#include <fiber/http/Http3Client.h>

#include <algorithm>
#include <new>
#include <string_view>
#include <utility>

#include <fiber/async/Spawn.h>
#include <fiber/common/Assert.h>
#include "http/Http3ClientConnectionImpl.h"

namespace fiber::http {

namespace {

constexpr std::string_view kHttp3Alpn = "h3";
constexpr std::uint64_t kHttp3PeerUnidirectionalStreamLimit = 16;
constexpr std::size_t kMaxCachedTokenLength = 16 * 1024;

} // namespace

Http3Client::Http3Client(quic::QuicUdpEndpoint &endpoint, Options options) noexcept :
    endpoint_(&endpoint), options_(std::move(options)), alpn_({kHttp3Alpn}) {}

common::IoResult<void> Http3Client::init() noexcept {
    if (initialized_ || endpoint_ == nullptr || !endpoint_->valid()) {
        return std::unexpected(initialized_ ? common::IoErr::Already : common::IoErr::Invalid);
    }
    initialized_ = true;
    return {};
}

Http3ClientConnectError Http3Client::make_error(Http3ClientConnectPhase phase, common::IoErr error) noexcept {
    return Http3ClientConnectError{.phase = phase, .io_error = error};
}

Http3ClientConnectError Http3Client::make_quic_error(const quic::QuicConnectError &error) noexcept {
    return Http3ClientConnectError{
            .phase = Http3ClientConnectPhase::Quic, .io_error = error.io_error, .quic_error = error};
}

async::Task<Http3ClientConnectResult> Http3Client::connect(Http3ClientConnectOptions options) noexcept {
    if (!initialized_ || endpoint_ == nullptr) {
        co_return std::unexpected(make_error(Http3ClientConnectPhase::ClientInit, common::IoErr::Invalid));
    }
    if (!endpoint_->running() || !endpoint_->loop().in_loop() || options.remote_addr.port() == 0 ||
        options.remote_addr.ip().is_unspecified() || options.handshake_timeout < std::chrono::milliseconds::zero()) {
        co_return std::unexpected(
                make_quic_error({.phase = quic::QuicConnectPhase::Endpoint, .io_error = common::IoErr::Invalid}));
    }

    const quic::QuicClientCacheKey cache_key{
            .server_name = options.server_name,
            .verify_name = options.verify_name,
            .remote_addr = options.remote_addr,
            .credential = options_.tls.credential,
            .trust_store = options_.tls.trust_store,
    };
    quic::QuicClientCachedState cached{};
    if (options_.cache.load != nullptr) {
        if (!options_.cache.load(options_.cache.owner, cache_key, cached)) {
            cached = {};
        }
        if ((cached.token == nullptr && cached.token_len != 0) || cached.token_len > kMaxCachedTokenLength) {
            co_return std::unexpected(
                    make_quic_error({.phase = quic::QuicConnectPhase::Connection, .io_error = common::IoErr::Invalid}));
        }
    }

    auto identity = endpoint_->allocate_client_identity();
    if (!identity) {
        co_return std::unexpected(
                make_quic_error({.phase = quic::QuicConnectPhase::Connection, .io_error = identity.error()}));
    }

    quic::QuicConnection::Options quic_options{};
    quic_options.role = quic::QuicConnectionRole::Client;
    quic_options.local_addr = endpoint_->local_addr();
    quic_options.remote_addr = options.remote_addr;
    quic_options.original_destination_connection_id = identity->original_destination_connection_id;
    quic_options.initial_destination_connection_id = identity->original_destination_connection_id;
    quic_options.remote_connection_id = identity->original_destination_connection_id;
    quic_options.local_connection_id = identity->local_connection_id;
    quic_options.transport = options.transport;
    quic_options.transport.initial_max_streams_bidi = 0;
    quic_options.transport.initial_max_streams_uni =
            std::max(quic_options.transport.initial_max_streams_uni, kHttp3PeerUnidirectionalStreamLimit);
    quic_options.keepalive_interval = options.keepalive_interval;
    quic_options.recv_flow = options.recv_flow;
    quic_options.max_peer_bidirectional_streams = 0;
    quic_options.max_peer_unidirectional_streams = kHttp3PeerUnidirectionalStreamLimit;
    // A client opens no local stream until the peer's transport parameters
    // arrive; HTTP/3 does not attempt 0-RTT, so nothing seeds these earlier.
    quic_options.max_local_bidirectional_streams = 0;
    quic_options.max_local_unidirectional_streams = 0;

    Http3ClientConnectionImpl::Options h3_options{};
    h3_options.local_settings = options_.local_settings;
    if (h3_options.local_settings.max_field_section_size == 0) {
        h3_options.local_settings.max_field_section_size = options_.max_field_section_size;
    }
    h3_options.drain_timeout = options_.drain_timeout;
    h3_options.max_qpack_string_size = options_.max_qpack_string_size;
    h3_options.max_field_section_size = options_.max_field_section_size;
    h3_options.server_name = options.server_name;
    h3_options.verify_name = options.verify_name;
    h3_options.remote_addr = options.remote_addr;
    h3_options.security = options_.tls;
    h3_options.cache = &options_.cache;

    auto *session = Http3ClientConnectionImpl::create(*endpoint_, quic_options, h3_options);
    if (session == nullptr) {
        co_return std::unexpected(make_error(Http3ClientConnectPhase::ClientInit, common::IoErr::NoMem));
    }
    quic::QuicConnection::Lease lease = quic::QuicConnection::Lease::adopt(&session->quic());
    quic::QuicConnection &connection = session->quic();

    // QUIC requires TLS 1.3; min/max are fixed here rather than caller-configurable.
    quic::QuicClientConnectParams params{};
    params.tls.security = options_.tls;
    params.tls.min_version = 0x0304;
    params.tls.max_version = 0x0304;
    params.tls.alpn = alpn_.view();
    params.tls.server_name = options.server_name;
    params.tls.verify_name = options.verify_name;
    params.allow_insecure = options.allow_insecure;
    params.resumption_session = cached.session;
    params.token = cached.token;
    params.token_len = cached.token_len;
    auto connected = connection.connect(params);
    if (!connected) {
        co_return std::unexpected(make_quic_error(connection.connect_error(connected.error())));
    }

    auto established = co_await connection.wait_established(options.handshake_timeout);
    if (!established) {
        Http3ClientConnectError error = make_quic_error(connection.connect_error(established.error()));
        if (!connection.terminal_closing()) {
            connection.close_immediately(quic::QuicErrorCode::NoError);
        }
        co_return std::unexpected(error);
    }
    if (connection.tls().selected_alpn() != kHttp3Alpn) {
        session->close(Http3ErrorCode::VersionFallback);
        co_return std::unexpected(make_error(Http3ClientConnectPhase::Alpn, common::IoErr::NotSupported));
    }

    auto h3_started = co_await session->start();
    if (!h3_started) {
        session->close(Http3ErrorCode::InternalError);
        co_return std::unexpected(make_error(Http3ClientConnectPhase::Http3, h3_started.error()));
    }

    co_return Http3ClientConnection(std::move(lease), *session);
}

} // namespace fiber::http
