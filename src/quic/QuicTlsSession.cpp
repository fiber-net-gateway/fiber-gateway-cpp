#include "QuicTlsSession.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>

#include <openssl/ssl.h>
#include <openssl/tls1.h>

#include "../net/TlsContext.h"
#include "QuicConnection.h"
#include "QuicCrypto.h"
#include "QuicCursor.h"
#include "QuicTransportParamsCodec.h"

namespace fiber::quic {

namespace {

[[nodiscard]] common::IoResult<QuicEncryptionLevel> quic_level_from_ssl(enum ssl_encryption_level_t level) noexcept {
    switch (level) {
        case ssl_encryption_initial:
            return QuicEncryptionLevel::Initial;
        case ssl_encryption_early_data:
            return QuicEncryptionLevel::EarlyData;
        case ssl_encryption_handshake:
            return QuicEncryptionLevel::Handshake;
        case ssl_encryption_application:
            return QuicEncryptionLevel::Application;
    }
    return std::unexpected(common::IoErr::Invalid);
}

[[nodiscard]] enum ssl_encryption_level_t ssl_level_from_quic(QuicEncryptionLevel level) noexcept {
    switch (level) {
        case QuicEncryptionLevel::Initial:
            return ssl_encryption_initial;
        case QuicEncryptionLevel::EarlyData:
            return ssl_encryption_early_data;
        case QuicEncryptionLevel::Handshake:
            return ssl_encryption_handshake;
        case QuicEncryptionLevel::Application:
            return ssl_encryption_application;
    }
    return ssl_encryption_application;
}

[[nodiscard]] common::IoResult<QuicCryptoSuite> crypto_suite_from_cipher(const SSL_CIPHER *cipher) noexcept {
    if (cipher == nullptr) {
        return std::unexpected(common::IoErr::Invalid);
    }

    switch (SSL_CIPHER_get_protocol_id(cipher)) {
        case TLS1_3_CK_AES_128_GCM_SHA256 & 0xffffU:
            return QuicCryptoSuite::Aes128GcmSha256;
        case TLS1_3_CK_AES_256_GCM_SHA384 & 0xffffU:
            return QuicCryptoSuite::Aes256GcmSha384;
        case TLS1_3_CK_CHACHA20_POLY1305_SHA256 & 0xffffU:
            return QuicCryptoSuite::ChaCha20Poly1305Sha256;
        default:
            return std::unexpected(common::IoErr::NotSupported);
    }
}

[[nodiscard]] QuicConnection *connection_from_ssl(SSL *ssl) noexcept {
    return ssl == nullptr ? nullptr : static_cast<QuicConnection *>(SSL_get_app_data(ssl));
}

int set_secret(SSL *ssl, enum ssl_encryption_level_t level, const SSL_CIPHER *cipher, const std::uint8_t *secret,
               std::size_t secret_len, bool write_secret) noexcept {
    QuicConnection *connection = connection_from_ssl(ssl);
    if (connection == nullptr || secret == nullptr || secret_len == 0) {
        return 0;
    }

    auto quic_level = quic_level_from_ssl(level);
    if (!quic_level || *quic_level == QuicEncryptionLevel::Initial) {
        return 0;
    }
    auto suite = crypto_suite_from_cipher(cipher);
    if (!suite) {
        return 0;
    }

    auto installed =
            quic_set_encryption_secret(connection->crypto(), *quic_level, write_secret, *suite, secret, secret_len);
    if (installed && *quic_level == QuicEncryptionLevel::Application &&
        connection->role() == QuicConnectionRole::Client && connection->crypto().application_read().ready() &&
        connection->crypto().application_write().ready()) {
        connection->crypto().discard_level(QuicEncryptionLevel::EarlyData);
    }
    return installed ? 1 : 0;
}

int set_read_secret(SSL *ssl, enum ssl_encryption_level_t level, const SSL_CIPHER *cipher, const std::uint8_t *secret,
                    std::size_t secret_len) noexcept {
    return set_secret(ssl, level, cipher, secret, secret_len, false);
}

int set_write_secret(SSL *ssl, enum ssl_encryption_level_t level, const SSL_CIPHER *cipher, const std::uint8_t *secret,
                     std::size_t secret_len) noexcept {
    return set_secret(ssl, level, cipher, secret, secret_len, true);
}

int add_handshake_data(SSL *ssl, enum ssl_encryption_level_t level, const std::uint8_t *data,
                       std::size_t len) noexcept {
    QuicConnection *connection = connection_from_ssl(ssl);
    if (connection == nullptr || (data == nullptr && len != 0)) {
        return 0;
    }

    if (!connection->peer_transport_params_received()) {
        const std::uint8_t *peer_params = nullptr;
        std::size_t peer_params_len = 0;
        SSL_get_peer_quic_transport_params(ssl, &peer_params, &peer_params_len);
        if (peer_params == nullptr || peer_params_len == 0) {
            connection->close(QuicErrorCode::TransportParameterError);
            return 0;
        }

        QuicTransportParams params{};
        QuicReadCursor reader(peer_params, peer_params_len);
        auto parsed = quic_parse_transport_params(QuicTransportParamOwner::Client, reader, params);
        if (!parsed) {
            connection->close(QuicErrorCode::TransportParameterError);
            return 0;
        }
        auto applied = connection->apply_peer_transport_params(params);
        if (!applied) {
            connection->close(QuicErrorCode::TransportParameterError);
            return 0;
        }
    }

    if (len == 0) {
        return 1;
    }

    auto quic_level = quic_level_from_ssl(level);
    if (!quic_level) {
        return 0;
    }

    QuicPacketNumberSpace &space = connection->packet_number_space(*quic_level);
    QuicOutputFrame *frame = space.alloc_frame();
    if (frame == nullptr) {
        connection->close(QuicErrorCode::InternalError);
        return 0;
    }

    frame->type = QuicFrameType::Crypto;
    frame->u.crypto.offset = space.crypto_sent;
    auto copied = quic_output_frame_set_owned_data(*frame, data, len);
    if (!copied) {
        space.release_frame(*frame);
        connection->close(QuicErrorCode::InternalError);
        return 0;
    }

    space.crypto_sent += len;
    space.pending_frames.push_back(*frame);
    return 1;
}

int flush_flight(SSL *ssl) noexcept { return connection_from_ssl(ssl) == nullptr ? 0 : 1; }

int send_alert(SSL *ssl, enum ssl_encryption_level_t level, std::uint8_t alert) noexcept {
    (void) level;
    QuicConnection *connection = connection_from_ssl(ssl);
    if (connection == nullptr) {
        return 0;
    }
    // Stash the alert; drive_handshake() converts it into a CRYPTO_ERROR close
    // once SSL_do_handshake returns. Mirrors nginx stashing qc->error in the
    // alert callback and emitting the close from the main loop, so connection
    // close state is never mutated re-entrantly from inside the TLS stack.
    connection->tls().record_alert(alert);
    return 1;
}

const SSL_QUIC_METHOD kQuicTlsMethod{
        .set_read_secret = set_read_secret,
        .set_write_secret = set_write_secret,
        .add_handshake_data = add_handshake_data,
        .flush_flight = flush_flight,
        .send_alert = send_alert,
};

struct QuicServerTransportParamsWire {
    std::size_t len = 0;
    std::size_t zero_rtt_len = 0;
};

[[nodiscard]] common::IoResult<QuicServerTransportParamsWire>
create_server_transport_params(QuicConnection &connection, std::uint8_t *out, std::size_t out_cap) noexcept {
    const QuicTransportSettings &settings = connection.local_transport();
    QuicTransportParams params{};
    params.max_idle_timeout = static_cast<std::uint64_t>(settings.max_idle_timeout.count());
    params.max_udp_payload_size = settings.max_udp_payload_size;
    params.initial_max_data = settings.initial_max_data;
    params.initial_max_stream_data_bidi_local = settings.initial_max_stream_data_bidi_local;
    params.initial_max_stream_data_bidi_remote = settings.initial_max_stream_data_bidi_remote;
    params.initial_max_stream_data_uni = settings.initial_max_stream_data_uni;
    params.initial_max_streams_bidi = settings.initial_max_streams_bidi;
    params.initial_max_streams_uni = settings.initial_max_streams_uni;
    params.ack_delay_exponent = settings.ack_delay_exponent;
    params.max_ack_delay = static_cast<std::uint64_t>(settings.max_ack_delay.count());
    params.active_connection_id_limit = settings.active_connection_id_limit;
    params.disable_active_migration = settings.disable_active_migration;
    params.has_original_destination_connection_id = true;
    params.original_destination_connection_id = connection.original_destination_connection_id();
    params.has_initial_source_connection_id = true;
    params.initial_source_connection_id = connection.local_connection_id();
    auto reset_token =
            connection.stateless_reset_token_for(connection.local_connection_id(), params.stateless_reset_token);
    if (reset_token) {
        params.has_stateless_reset_token = true;
    }
    if (connection.retried()) {
        params.has_retry_source_connection_id = true;
        params.retry_source_connection_id = connection.retry_source_connection_id();
    }

    std::size_t zero_rtt_len = 0;
    QuicWriteCursor writer(out, out_cap);
    auto len = quic_create_transport_params(QuicTransportParamOwner::Server, &writer, params, &zero_rtt_len);
    if (!len) {
        return std::unexpected(len.error());
    }
    return QuicServerTransportParamsWire{.len = *len, .zero_rtt_len = zero_rtt_len};
}

} // namespace

QuicTlsSession::~QuicTlsSession() {
    if (ssl_ != nullptr) {
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
}

common::IoResult<void> QuicTlsSession::init_server(net::TlsServerContext &context,
                                                   QuicConnection &connection) noexcept {
    if (ssl_ != nullptr) {
        return std::unexpected(common::IoErr::Already);
    }

    SSL_CTX *ctx = context.raw();
    if (ctx == nullptr) {
        return std::unexpected(common::IoErr::Invalid);
    }

    SSL *ssl = SSL_new(ctx);
    if (ssl == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }

    if (SSL_set_quic_method(ssl, &kQuicTlsMethod) != 1) {
        SSL_free(ssl);
        return std::unexpected(common::IoErr::Invalid);
    }
    SSL_set_app_data(ssl, &connection);
    SSL_set_accept_state(ssl);

    auto bound = context.bind_quic_ssl(ssl, &connection.local_addr(), &connection.remote_addr());
    if (!bound) {
        SSL_free(ssl);
        return std::unexpected(bound.error());
    }

    std::array<std::uint8_t, 512> transport_params{};
    auto transport_params_wire =
            create_server_transport_params(connection, transport_params.data(), transport_params.size());
    if (!transport_params_wire) {
        SSL_free(ssl);
        return std::unexpected(transport_params_wire.error());
    }
    if (SSL_set_quic_transport_params(ssl, transport_params.data(), transport_params_wire->len) != 1) {
        SSL_free(ssl);
        return std::unexpected(common::IoErr::Invalid);
    }
    if (connection.early_data_enabled()) {
        if (transport_params_wire->zero_rtt_len == 0) {
            SSL_free(ssl);
            return std::unexpected(common::IoErr::Invalid);
        }
        SSL_set_early_data_enabled(ssl, 1);
        if (SSL_set_quic_early_data_context(ssl, transport_params.data(), transport_params_wire->zero_rtt_len) != 1) {
            SSL_free(ssl);
            return std::unexpected(common::IoErr::Invalid);
        }
    }

    ssl_ = ssl;
    return {};
}

common::IoResult<void> QuicTlsSession::provide_crypto_data(QuicEncryptionLevel level, const std::uint8_t *data,
                                                           std::size_t len) noexcept {
    if (ssl_ == nullptr || (data == nullptr && len != 0)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (SSL_provide_quic_data(ssl_, ssl_level_from_quic(level), data, len) != 1) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return {};
}

common::IoResult<void> QuicTlsSession::drive_handshake() noexcept {
    if (ssl_ == nullptr) {
        return std::unexpected(common::IoErr::Invalid);
    }

    const int rc = SSL_do_handshake(ssl_);
    if (rc == 1) {
        return {};
    }

    const int err = SSL_get_error(ssl_, rc);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        return std::unexpected(common::IoErr::WouldBlock);
    }

    // A TLS alert was raised during the handshake (captured by the send_alert
    // callback). RFC 9000 §20.1: communicate it to the peer as a
    // CONNECTION_CLOSE carrying CRYPTO_ERROR = 0x0100 | alert, instead of
    // collapsing every handshake failure to a generic error. The general
    // 0x0100 | alert mapping already encodes the special alerts nginx
    // special-cases (no_application_protocol=120, missing_extension=109).
    if (auto alert = take_pending_alert()) {
        if (QuicConnection *connection = connection_from_ssl(ssl_)) {
            connection->close_crypto_error(*alert);
        }
    }
    return std::unexpected(common::IoErr::Invalid);
}

bool QuicTlsSession::handshake_done() const noexcept { return ssl_ != nullptr && SSL_is_init_finished(ssl_) == 1; }

void QuicTlsSession::record_alert(std::uint8_t alert) noexcept { pending_alert_ = alert; }

std::optional<std::uint8_t> QuicTlsSession::take_pending_alert() noexcept {
    std::optional<std::uint8_t> alert = pending_alert_;
    pending_alert_.reset();
    return alert;
}

} // namespace fiber::quic
