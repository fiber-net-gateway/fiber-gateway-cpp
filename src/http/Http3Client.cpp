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

} // namespace

Http3Client::Http3Client(quic::QuicUdpEndpoint &endpoint, Options options) noexcept :
    endpoint_(&endpoint), options_(std::move(options)) {}

common::IoResult<void> Http3Client::init() noexcept {
    if (initialized_ || endpoint_ == nullptr || !endpoint_->valid()) {
        return std::unexpected(initialized_ ? common::IoErr::Already : common::IoErr::Invalid);
    }
    quic::QuicClient::Options client_options{};
    client_options.connection_owner = this;
    client_options.create_connection = &Http3Client::create_connection_op;
    client_options.cache = options_.cache;
    client_options.alpn = {kHttp3Alpn};
    auto client_initialized = quic_client_.init(*endpoint_, options_.tls, std::move(client_options));
    if (!client_initialized) {
        return std::unexpected(client_initialized.error());
    }
    initialized_ = true;
    return {};
}

quic::QuicConnection::Lease Http3Client::create_connection_op(void *owner, quic::QuicUdpEndpoint &endpoint,
                                                              const quic::QuicConnection::Options &options) noexcept {
    auto *client = static_cast<Http3Client *>(owner);
    return client == nullptr ? quic::QuicConnection::Lease{} : client->create_connection(endpoint, options);
}

quic::QuicConnection::Lease Http3Client::create_connection(quic::QuicUdpEndpoint &endpoint,
                                                           const quic::QuicConnection::Options &options) noexcept {
    FIBER_ASSERT(&endpoint == endpoint_);
    Http3ClientConnectionImpl::Options h3_options{};
    h3_options.local_settings = options_.local_settings;
    if (h3_options.local_settings.max_field_section_size == 0) {
        h3_options.local_settings.max_field_section_size = options_.max_field_section_size;
    }
    h3_options.drain_timeout = options_.drain_timeout;
    h3_options.max_qpack_string_size = options_.max_qpack_string_size;
    h3_options.max_field_section_size = options_.max_field_section_size;
    auto *session = Http3ClientConnectionImpl::create(endpoint, options, h3_options);
    if (session == nullptr) {
        return {};
    }
    last_created_connection_ = session;
    return quic::QuicConnection::Lease::adopt(&session->quic());
}

Http3ClientConnectError Http3Client::make_error(Http3ClientConnectPhase phase, common::IoErr error) noexcept {
    return Http3ClientConnectError{.phase = phase, .io_error = error};
}

async::Task<Http3ClientConnectResult> Http3Client::connect(Http3ClientConnectOptions options) noexcept {
    if (!initialized_ || endpoint_ == nullptr) {
        co_return std::unexpected(make_error(Http3ClientConnectPhase::ClientInit, common::IoErr::Invalid));
    }

    quic::QuicClientConnectOptions quic_options{};
    quic_options.remote_addr = options.remote_addr;
    quic_options.server_name = std::move(options.server_name);
    quic_options.verify_name = std::move(options.verify_name);
    quic_options.transport = options.transport;
    quic_options.recv_flow = options.recv_flow;
    quic_options.keepalive_interval = options.keepalive_interval;
    quic_options.handshake_timeout = options.handshake_timeout;
    quic_options.max_peer_bidirectional_streams = 0;
    quic_options.max_peer_unidirectional_streams = kHttp3PeerUnidirectionalStreamLimit;
    quic_options.transport.initial_max_streams_bidi = 0;
    quic_options.transport.initial_max_streams_uni =
            std::max(quic_options.transport.initial_max_streams_uni, kHttp3PeerUnidirectionalStreamLimit);
    quic_options.allow_insecure = options.allow_insecure;

    last_created_connection_ = nullptr;
    auto started = quic_client_.start_connect(quic_options);
    Http3ClientConnectionImpl *session = last_created_connection_;
    last_created_connection_ = nullptr;
    if (!started) {
        Http3ClientConnectError error = make_error(Http3ClientConnectPhase::Quic, started.error().io_error);
        error.quic_error = started.error();
        co_return std::unexpected(error);
    }
    if (session == nullptr) {
        started->cancel();
        co_return std::unexpected(make_error(Http3ClientConnectPhase::ClientInit, common::IoErr::Invalid));
    }

    quic::QuicClientAttempt attempt = std::move(*started);
    auto connected = co_await attempt.wait_connected();
    if (!connected) {
        Http3ClientConnectError error = make_error(Http3ClientConnectPhase::Quic, connected.error().io_error);
        error.quic_error = connected.error();
        co_return std::unexpected(error);
    }
    if (attempt.connection() == nullptr || attempt.connection()->tls().selected_alpn() != kHttp3Alpn) {
        session->close(Http3ErrorCode::VersionFallback);
        co_return std::unexpected(make_error(Http3ClientConnectPhase::Alpn, common::IoErr::NotSupported));
    }

    auto h3_started = co_await session->start();
    if (!h3_started) {
        session->close(Http3ErrorCode::InternalError);
        co_return std::unexpected(make_error(Http3ClientConnectPhase::Http3, h3_started.error()));
    }

    co_return Http3ClientConnection(attempt.release(), *session);
}

} // namespace fiber::http
