#include "Http3Client.h"

#include <algorithm>
#include <new>
#include <string_view>
#include <utility>

#include "../async/Spawn.h"
#include "../common/Assert.h"
#include "TlsAlpn.h"

namespace fiber::http {

namespace {

constexpr std::string_view kHttp3Alpn = "h3";
constexpr std::uint64_t kHttp3PeerUnidirectionalStreamLimit = 16;

} // namespace

class Http3Client::Session final : public common::NonCopyable, public common::NonMovable {
public:
    Session(const quic::QuicConnection::Options &quic_options, const Http3Connection::Options &h3_options) noexcept :
        quic_(make_quic_options(quic_options, this)), h3_(quic_, h3_options) {
        prepared_ = h3_.prepare().has_value();
    }

    [[nodiscard]] quic::QuicConnection &quic() noexcept { return quic_; }
    [[nodiscard]] Http3Connection &h3() noexcept { return h3_; }
    [[nodiscard]] bool prepared() const noexcept { return prepared_; }

private:
    [[nodiscard]] static quic::QuicConnection::Options make_quic_options(const quic::QuicConnection::Options &base,
                                                                         Session *owner) noexcept {
        quic::QuicConnection::Options options = base;
        options.destroy_owner = owner;
        options.on_destroy = &Session::destroy_connection;
        return options;
    }

    static void destroy_connection(void *owner, quic::QuicConnection &connection) noexcept {
        auto *session = static_cast<Session *>(owner);
        if (session == nullptr || session->cleanup_started_) {
            return;
        }
        session->cleanup_started_ = true;
        event::EventLoop *loop = connection.loop();
        if (loop == nullptr) {
            delete session;
            return;
        }
        async::spawn(*loop, [session]() -> async::DetachedTask {
            co_await session->h3_.wait_closed();
            delete session;
            co_return;
        });
    }

    quic::QuicConnection quic_;
    Http3Connection h3_;
    bool prepared_ = false;
    bool cleanup_started_ = false;
};

net::TlsOptions Http3Client::normalize_tls_options(net::TlsOptions options, bool verify_peer) noexcept {
    options.enabled = true;
    options.verify_peer = verify_peer;
    options.min_version = 0x0304;
    options.max_version = 0x0304;
    normalize_http3_alpn(options);
    return options;
}

Http3Client::Http3Client(quic::QuicUdpEndpoint &endpoint, Options options) noexcept :
    endpoint_(&endpoint), options_(std::move(options)),
    tls_context_(normalize_tls_options(options_.tls, options_.verify_peer), false) {}

common::IoResult<void> Http3Client::init() noexcept {
    if (initialized_ || endpoint_ == nullptr || !endpoint_->valid()) {
        return std::unexpected(initialized_ ? common::IoErr::Already : common::IoErr::Invalid);
    }
    auto tls_initialized = tls_context_.init();
    if (!tls_initialized) {
        return std::unexpected(tls_initialized.error());
    }

    quic::QuicClient::Options client_options{};
    client_options.connection_owner = this;
    client_options.create_connection = &Http3Client::create_connection_op;
    client_options.cache = options_.cache;
    auto client_initialized = quic_client_.init(*endpoint_, tls_context_, client_options);
    if (!client_initialized) {
        return std::unexpected(client_initialized.error());
    }
    initialized_ = true;
    return {};
}

Http3Connection::Options Http3Client::make_h3_options() const noexcept {
    Http3Connection::Options options{};
    options.local_settings = options_.local_settings;
    if (options.local_settings.max_field_section_size == 0) {
        options.local_settings.max_field_section_size = options_.max_field_section_size;
    }
    options.drain_timeout = options_.drain_timeout;
    options.max_qpack_string_size = options_.max_qpack_string_size;
    options.max_field_section_size = options_.max_field_section_size;
    options.enable_push = false;
    return options;
}

quic::QuicConnection::Lease Http3Client::create_connection_op(void *owner,
                                                              const quic::QuicConnection::Options &options) noexcept {
    auto *client = static_cast<Http3Client *>(owner);
    return client == nullptr ? quic::QuicConnection::Lease{} : client->create_connection(options);
}

quic::QuicConnection::Lease Http3Client::create_connection(const quic::QuicConnection::Options &options) noexcept {
    auto *session = new (std::nothrow) Session(options, make_h3_options());
    if (session == nullptr) {
        return {};
    }
    if (!session->prepared()) {
        delete session;
        return {};
    }
    last_created_session_ = session;
    return quic::QuicConnection::Lease::adopt(&session->quic());
}

Http3ClientConnectError Http3Client::make_error(Http3ClientConnectPhase phase, common::IoErr error) noexcept {
    return Http3ClientConnectError{.phase = phase, .io_error = error};
}

async::Task<Http3ClientConnectResult> Http3Client::connect(quic::QuicClientConnectOptions options) noexcept {
    if (!initialized_ || endpoint_ == nullptr) {
        co_return std::unexpected(make_error(Http3ClientConnectPhase::ClientInit, common::IoErr::Invalid));
    }

    options.enable_early_data = false;
    options.application_owner = nullptr;
    options.application_ops = {};
    options.max_peer_bidirectional_streams = 0;
    options.max_peer_unidirectional_streams = kHttp3PeerUnidirectionalStreamLimit;
    options.transport.initial_max_streams_bidi = 0;
    options.transport.initial_max_streams_uni =
            std::max(options.transport.initial_max_streams_uni, kHttp3PeerUnidirectionalStreamLimit);

    last_created_session_ = nullptr;
    auto started = quic_client_.start_connect(options);
    Session *session = last_created_session_;
    last_created_session_ = nullptr;
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
        session->h3().close(Http3ErrorCode::VersionFallback);
        co_return std::unexpected(make_error(Http3ClientConnectPhase::Alpn, common::IoErr::NotSupported));
    }

    auto h3_started = co_await session->h3().start();
    if (!h3_started) {
        session->h3().close(Http3ErrorCode::InternalError);
        co_return std::unexpected(make_error(Http3ClientConnectPhase::Http3, h3_started.error()));
    }

    co_return Http3ClientConnection(attempt.release(), session->h3());
}

} // namespace fiber::http
