#include <fiber/quic/QuicTlsSession.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <utility>

#include <fiber/common/Assert.h>
#include <fiber/net/TlsServerHandshakeConfig.h>
#include <fiber/net/TrustStore.h>
#include <fiber/net/detail/TlsClientStaging.h>
#include <fiber/quic/QuicConnection.h>
#include <fiber/quic/QuicCursor.h>
#include <fiber/quic/QuicFrame.h>
#include <fiber/tls/TlsTicketService.h>
#include <fiber/tls/crypto/Tls13KeySchedule.h>
#include "quic/QuicCrypto.h"
#include "quic/QuicTransportParamsCodec.h"
#include "tls/handshake/TlsClientHandshakeEngine.h"
#include "tls/handshake/TlsServerHandshakeEngine.h"

namespace fiber::quic {

namespace {

using net::detail::system_now_unix_ms;

// Post-handshake CRYPTO is a raw handshake-message stream (10 §7). One bound
// covers the buffered tail and each message: NSTs are the only legal content
// and real tickets are a few hundred bytes.
constexpr std::size_t kMaxPostHandshakeBytes = 64 * 1024;
constexpr std::size_t kTlsHandshakeHeaderSize = 4;

[[nodiscard]] tls::TlsQuicLevel tls_level_from_encryption(QuicEncryptionLevel level) noexcept {
    switch (level) {
        case QuicEncryptionLevel::Initial:
            return tls::TlsQuicLevel::Initial;
        case QuicEncryptionLevel::EarlyData:
            return tls::TlsQuicLevel::EarlyData;
        case QuicEncryptionLevel::Handshake:
            return tls::TlsQuicLevel::Handshake;
        case QuicEncryptionLevel::Application:
            return tls::TlsQuicLevel::Application;
    }
    return tls::TlsQuicLevel::Application;
}

[[nodiscard]] QuicEncryptionLevel encryption_level_from_tls(tls::TlsQuicLevel level) noexcept {
    switch (level) {
        case tls::TlsQuicLevel::Initial:
            return QuicEncryptionLevel::Initial;
        case tls::TlsQuicLevel::EarlyData:
            return QuicEncryptionLevel::EarlyData;
        case tls::TlsQuicLevel::Handshake:
            return QuicEncryptionLevel::Handshake;
        case tls::TlsQuicLevel::Application:
            return QuicEncryptionLevel::Application;
    }
    return QuicEncryptionLevel::Application;
}

[[nodiscard]] std::optional<QuicCryptoSuite> quic_suite_from_tls(tls::TlsCipherSuiteId suite) noexcept {
    switch (suite) {
        case tls::TlsCipherSuiteId::TlsAes128GcmSha256:
            return QuicCryptoSuite::Aes128GcmSha256;
        case tls::TlsCipherSuiteId::TlsAes256GcmSha384:
            return QuicCryptoSuite::Aes256GcmSha384;
        case tls::TlsCipherSuiteId::TlsChacha20Poly1305Sha256:
            return QuicCryptoSuite::ChaCha20Poly1305Sha256;
        default:
            // 1.2-only suites can't be negotiated with the engines pinned to
            // 1.3 in QUIC mode.
            return std::nullopt;
    }
}

// RFC 8446 §4.6.1 NewSessionTicket body: u32 lifetime + u32 ticket_age_add +
// opaque nonce<0..255> + opaque ticket<1..65535> + extensions. Only the
// early_data extension (42, u32) is defined; anything unknown is skipped per
// §4.1.1 extension tolerance (the receipt path stores no unknown state).
struct NstDecoded {
    std::uint32_t lifetime_s = 0;
    std::uint32_t ticket_age_add = 0;
    std::span<const std::uint8_t> nonce{};
    std::span<const std::uint8_t> ticket{};
    std::uint32_t max_early_data = 0;
};

[[nodiscard]] bool decode_new_session_ticket(std::span<const std::uint8_t> body, NstDecoded &out) noexcept {
    if (body.size() < 11) { // 4 + 4 + 1 + 2 + 0 (empty extension block)
        return false;
    }
    std::size_t off = 0;
    const auto be16 = [&body](std::size_t at) noexcept {
        return static_cast<std::uint16_t>((static_cast<std::uint16_t>(body[at]) << 8) | body[at + 1]);
    };
    const auto be32 = [&body](std::size_t at) noexcept {
        return (static_cast<std::uint32_t>(body[at]) << 24) | (static_cast<std::uint32_t>(body[at + 1]) << 16) |
               (static_cast<std::uint32_t>(body[at + 2]) << 8) | static_cast<std::uint32_t>(body[at + 3]);
    };
    out.lifetime_s = be32(off);
    off += 4;
    out.ticket_age_add = be32(off);
    off += 4;
    const std::size_t nonce_len = body[off++];
    if (off + nonce_len + 2 > body.size()) {
        return false;
    }
    out.nonce = body.subspan(off, nonce_len);
    off += nonce_len;
    const std::size_t ticket_len = be16(off);
    off += 2;
    if (off + ticket_len + 2 > body.size() || ticket_len == 0) {
        return false;
    }
    out.ticket = body.subspan(off, ticket_len);
    off += ticket_len;
    const std::size_t ext_block = be16(off);
    off += 2;
    if (off + ext_block != body.size()) {
        return false;
    }
    const std::size_t ext_end = off + ext_block;
    while (off + 4 <= ext_end) {
        const std::uint16_t type = be16(off);
        const std::uint16_t len = be16(off + 2);
        off += 4;
        if (off + len > ext_end) {
            return false;
        }
        if (type == 42 && len == 4) { // early_data
            out.max_early_data = be32(off);
        }
        off += len;
    }
    return off == ext_end;
}

struct QuicServerTransportParamsWire {
    std::size_t len = 0;
    std::size_t zero_rtt_len = 0; // the leading slice governing early data — the 0-RTT gate's binding
};

[[nodiscard]] common::IoResult<std::size_t>
create_client_transport_params(QuicConnection &connection, std::uint8_t *out, std::size_t out_cap) noexcept {
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
    params.has_initial_source_connection_id = true;
    params.initial_source_connection_id = connection.local_connection_id();

    QuicWriteCursor writer(out, out_cap);
    return quic_create_transport_params(QuicTransportParamOwner::Client, &writer, params);
}

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

    QuicWriteCursor writer(out, out_cap);
    std::size_t zero_rtt_len = 0;
    auto len = quic_create_transport_params(QuicTransportParamOwner::Server, &writer, params, &zero_rtt_len);
    if (!len) {
        return std::unexpected(len.error());
    }
    return QuicServerTransportParamsWire{.len = *len, .zero_rtt_len = zero_rtt_len};
}

// Zero-config ticket parity (10 §8): the pre-10 BoringSSL path minted NSTs
// from the process-shared SSL_CTX's default ticket keys, so every
// server connection in the process could open each other's tickets. A param
// without ticket_service keeps that shape: one process-wide random key, born
// at first use, minting for the process lifetime (like the system trust
// store, intentionally never destroyed; 08's injected-rotation contract
// remains the deployment path). An entropy failure leaves null — minter and
// lookup stay unwired, the documented safe degradation (no NST, no
// resumption).
const tls::TlsTicketService *default_ticket_service() noexcept {
    const auto build = []() noexcept -> tls::TlsTicketService * {
        std::array<tls::TlsTicketKeyMaterial, 1> keys{};
        if (!tls::TlsTicketService::random_key(1, system_now_unix_ms(), keys[0])) {
            return nullptr;
        }
        constexpr std::uint32_t kTenYears = 10u * 365u * 86400u;
        const tls::TlsTicketKeyPolicy policy{.key_lifetime_s = kTenYears, .key_retention_s = kTenYears};
        return new (std::nothrow) tls::TlsTicketService(keys, policy);
    };
    static tls::TlsTicketService *const held = build();
    return held != nullptr && held->valid() ? held : nullptr;
}

} // namespace

QuicTlsSession::~QuicTlsSession() {
    // The engines' chains resolve the current loop's node pool — this runs on
    // the connection's loop, where the connection (and its session member)
    // die.
    delete client_;
    delete server_;
    client_ = nullptr;
    server_ = nullptr;
}

// =====================================================================
// callback trampolines
// =====================================================================

bool QuicTlsSession::quic_set_secret_thunk(void *ctx, tls::TlsQuicLevel level, bool write_secret,
                                           tls::TlsCipherSuiteId suite, std::span<const std::uint8_t> secret) noexcept {
    return static_cast<QuicTlsSession *>(ctx)->on_quic_set_secret(level, write_secret, suite, secret);
}

bool QuicTlsSession::quic_add_data_thunk(void *ctx, tls::TlsQuicLevel level,
                                         std::span<const std::uint8_t> data) noexcept {
    return static_cast<QuicTlsSession *>(ctx)->on_quic_add_data(level, data);
}

void QuicTlsSession::quic_peer_params_thunk(void *ctx, std::span<const std::uint8_t> params) noexcept {
    static_cast<QuicTlsSession *>(ctx)->on_quic_peer_params(params);
}

void QuicTlsSession::quic_send_alert_thunk(void *ctx, tls::TlsAlertDesc alert) noexcept {
    static_cast<QuicTlsSession *>(ctx)->on_quic_alert(alert);
}

// =====================================================================
// init — config staging + engine construction (10 §8)
// =====================================================================

common::IoResult<void> QuicTlsSession::init_server(const net::TlsServerParam &options,
                                                   QuicConnection &connection) noexcept {
    if (initialized()) {
        return std::unexpected(common::IoErr::Already);
    }
    if (!options.enabled()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    // ALPN is mandatory in QUIC (RFC 9001 §8.1) — the engine would answer
    // no_application_protocol to every client; fail the configuration instead.
    if (options.alpn.empty()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (options.client_certificate_mode != net::TlsClientCertificateMode::None && options.trust_store == nullptr) {
        return std::unexpected(common::IoErr::Invalid);
    }

    connection_ = &connection;
    auto transport_params_wire =
            create_server_transport_params(connection, local_transport_params_.data(), local_transport_params_.size());
    if (!transport_params_wire) {
        connection_ = nullptr;
        return std::unexpected(transport_params_wire.error());
    }
    local_transport_params_len_ = transport_params_wire->len;

    server_cfg_ = tls::TlsServerConfig{};
    if (options.trust_store != nullptr) {
        server_cfg_.client_trust = &options.trust_store->tls_store();
    }
    server_cfg_.require_client_cert = options.client_certificate_mode == net::TlsClientCertificateMode::Required;
    server_cfg_.alpn = options.alpn;
    server_cfg_.min_version = tls::kTlsVersionTls13; // QUIC is TLS 1.3 only (10 §3.4)
    server_cfg_.max_version = tls::kTlsVersionTls13;
    server_cfg_.enable_early_data = options.enable_early_data;
    server_cfg_.now_unix_ms = system_now_unix_ms();
    server_cfg_.quic = &quic_cb_;
    server_cfg_.quic_transport_params = {local_transport_params_.data(), local_transport_params_len_};
    // The 0-RTT consistency gate's binding (10 §6.1): the codec guarantees the
    // early-data-relevant params sit in the leading slice, so the context is a
    // prefix view of the same stable buffer the EE extension borrows.
    server_cfg_.quic_early_data_context = {local_transport_params_.data(), transport_params_wire->zero_rtt_len};

    quic_cb_ = tls::TlsQuicCallbacks{.set_secret = &QuicTlsSession::quic_set_secret_thunk,
                                     .add_handshake_data = &QuicTlsSession::quic_add_data_thunk,
                                     .on_peer_transport_params = &QuicTlsSession::quic_peer_params_thunk,
                                     .send_alert = &QuicTlsSession::quic_send_alert_thunk,
                                     .ctx = this};

    server_param_ = &options;
    selector_.select = [](void *ctx, const tls::TlsClientHello &client_hello) noexcept -> const tls::TlsServerConfig * {
        return static_cast<QuicTlsSession *>(ctx)->select_server_config(client_hello);
    };
    selector_.ctx = this;
    // Session tickets (09 §6 / 10 §8): the service's adapters are staged here
    // so the engine borrows them for the handshake. Unconfigured falls back to
    // the process-wide default service (pre-10 parity); only an entropy
    // failure leaves null — no NST, no resumption, which the engine treats
    // natively.
    const tls::TlsTicketService *tickets =
            options.ticket_service != nullptr ? options.ticket_service : default_ticket_service();
    if (tickets != nullptr) {
        minter_ = tickets->minter();
        lookup_ = tickets->lookup();
    }
    // Credentials arrive per ClientHello through the selector, so the
    // template config passes only the selector-mode invariant checks.
    server_ = new (std::nothrow) tls::TlsServerHandshakeEngine(server_cfg_, tickets != nullptr ? &lookup_ : nullptr,
                                                               tickets != nullptr ? &minter_ : nullptr, &selector_);
    if (server_ == nullptr) {
        connection_ = nullptr;
        return std::unexpected(common::IoErr::NoMem);
    }
    if (server_->failed()) { // construction-terminal (config invariant)
        record_alert(static_cast<std::uint8_t>(server_->failure_alert()));
        delete server_;
        server_ = nullptr;
        connection_ = nullptr;
        return std::unexpected(common::IoErr::Invalid);
    }
    return {};
}

const tls::TlsServerConfig *QuicTlsSession::select_server_config(const tls::TlsClientHello &client_hello) noexcept {
    // The 09 §4.1 shim (TlsStreamFd::select_server_config's QUIC twin): the
    // net configure callback stages credentials into the template config
    // under the documented param borrow; its error latches for
    // drive_handshake to report after the engine's handshake_failure.
    callback_error_ = common::IoErr::None;
    server_cfg_.chain = nullptr;
    server_cfg_.key = nullptr;
    credential_owner_.reset();
    std::size_t credential_count = 0;
    net::TlsServerHandshakeConfig config(server_cfg_, credential_count, credential_owner_);
    const tls::TlsClientHelloView view = client_hello.view();
    common::IoErr error = server_param_->configure_callback(server_param_->configure_ctx, config, view);
    if (error == common::IoErr::None && credential_count == 0) {
        error = common::IoErr::Invalid;
    }
    if (error != common::IoErr::None) {
        credential_owner_.reset(); // nothing will read it: release now
        callback_error_ = error;
        return nullptr; // the engine answers handshake_failure
    }
    return &server_cfg_;
}

common::IoResult<void> QuicTlsSession::init_client(const net::TlsClientParam &param, QuicConnection &connection,
                                                   bool allow_insecure, const tls::TlsSessionState *session) noexcept {
    if (initialized() || param.alpn.empty()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    const bool verify_peer = param.security.verify_peer;
    if (!verify_peer && !allow_insecure) {
        return std::unexpected(common::IoErr::Permission);
    }
    if (verify_peer && param.server_name.empty() && param.verify_name.empty()) {
        return std::unexpected(common::IoErr::Invalid);
    }

    connection_ = &connection;
    auto transport_params_len =
            create_client_transport_params(connection, local_transport_params_.data(), local_transport_params_.size());
    if (!transport_params_len) {
        connection_ = nullptr;
        return std::unexpected(transport_params_len.error());
    }
    local_transport_params_len_ = *transport_params_len;

    client_cfg_ = tls::TlsClientConfig{};
    if (auto staged = net::detail::TlsClientStager::stage(param, client_cfg_, ip_bytes_); !staged) {
        connection_ = nullptr;
        return std::unexpected(staged.error());
    }
    client_cfg_.min_version = tls::kTlsVersionTls13; // QUIC is TLS 1.3 only (10 §3.4)
    client_cfg_.max_version = tls::kTlsVersionTls13;
    client_cfg_.quic = &quic_cb_;
    client_cfg_.quic_transport_params = {local_transport_params_.data(), local_transport_params_len_};

    quic_cb_ = tls::TlsQuicCallbacks{.set_secret = &QuicTlsSession::quic_set_secret_thunk,
                                     .add_handshake_data = &QuicTlsSession::quic_add_data_thunk,
                                     .on_peer_transport_params = &QuicTlsSession::quic_peer_params_thunk,
                                     .send_alert = &QuicTlsSession::quic_send_alert_thunk,
                                     .ctx = this};

    // Resumption staging (10 §6.2): the borrowed receipt is copied into the
    // session (identity vector + TlsSecret are owning), and the offer spans
    // point at that copy — the param's session only lives until connect()
    // returns, the handshake continues long after.
    if (session != nullptr && !session->empty()) {
        session_state_.identity = session->identity;
        session_state_.psk = tls::TlsSecret::from_bytes(session->psk.bytes());
        session_state_.suite = session->suite;
        session_state_.ticket_age_add = session->ticket_age_add;
        session_state_.ticket_lifetime_s = session->ticket_lifetime_s;
        session_state_.max_early_data = session->max_early_data;
        session_state_.alpn = session->alpn;
        session_state_.alpn_len = session->alpn_len;
        session_state_.issued_ms = session->issued_ms;
        offer_.identity = session_state_.identity;
        offer_.obfuscated_ticket_age = session_state_.obfuscated_ticket_age(client_cfg_.now_unix_ms);
        offer_.suite = session_state_.suite;
        offer_.psk = session_state_.psk.bytes();
        offer_.max_early_data = session_state_.max_early_data;
    }

    client_mode_ = true;
    verify_peer_ = verify_peer;
    // Construction emits the ClientHello through the callbacks (and, with an
    // offer, exports the early write secret for 0-RTT packets) — the Initial
    // keys and packet spaces must already exist, which connect() guarantees.
    client_ = new (std::nothrow) tls::TlsClientHandshakeEngine(client_cfg_, session_state_.empty() ? nullptr : &offer_);
    if (client_ == nullptr) {
        connection_ = nullptr;
        return std::unexpected(common::IoErr::NoMem);
    }
    if (client_->failed()) { // construction-terminal (entropy / allocation)
        record_alert(static_cast<std::uint8_t>(client_->failure_alert()));
        delete client_;
        client_ = nullptr;
        connection_ = nullptr;
        return std::unexpected(common::IoErr::Invalid);
    }
    return {};
}

// =====================================================================
// callbacks → QUIC layer
// =====================================================================

bool QuicTlsSession::on_quic_set_secret(tls::TlsQuicLevel level, bool write_secret, tls::TlsCipherSuiteId suite,
                                        std::span<const std::uint8_t> secret) noexcept {
    if (connection_ == nullptr || secret.empty() || level == tls::TlsQuicLevel::Initial) {
        return false;
    }
    const auto quic_suite = quic_suite_from_tls(suite);
    if (!quic_suite) {
        return false;
    }
    negotiated_suite_ = suite; // the hs/app exports carry the negotiated suite
    const QuicEncryptionLevel quic_level = encryption_level_from_tls(level);
    auto installed = quic_set_encryption_secret(connection_->crypto(), quic_level, write_secret, *quic_suite,
                                                secret.data(), secret.size());
    if (installed && quic_level == QuicEncryptionLevel::Application &&
        connection_->role() == QuicConnectionRole::Client && connection_->crypto().application_read().ready() &&
        connection_->crypto().application_write().ready()) {
        connection_->crypto().discard_level(QuicEncryptionLevel::EarlyData);
    }
    return installed.has_value();
}

bool QuicTlsSession::on_quic_add_data(tls::TlsQuicLevel level, std::span<const std::uint8_t> data) noexcept {
    if (connection_ == nullptr || (data.data() == nullptr && !data.empty())) {
        return false;
    }
    if (data.empty()) {
        return true;
    }
    const QuicEncryptionLevel quic_level = encryption_level_from_tls(level);
    QuicPacketNumberSpace &space = connection_->packet_number_space(quic_level);
    QuicOutputFrame *frame = space.alloc_frame();
    if (frame == nullptr) {
        connection_->close(QuicErrorCode::InternalError);
        return false;
    }
    frame->type = QuicFrameType::Crypto;
    frame->u.crypto.offset = space.crypto_sent;
    auto copied = quic_output_frame_set_owned_data(*frame, data.data(), data.size());
    if (!copied) {
        space.release_frame(*frame);
        connection_->close(QuicErrorCode::InternalError);
        return false;
    }
    space.crypto_sent += data.size();
    space.pending_frames.push_back(*frame);
    return true;
}

void QuicTlsSession::on_quic_peer_params(std::span<const std::uint8_t> params) noexcept {
    if (params.size() > peer_transport_params_.size()) {
        // Oversized (or absent-but-hostile) parameters fail the handshake's
        // completeness check as TransportParameterError rather than here —
        // this callback cannot close the connection re-entrantly.
        peer_transport_params_overflow_ = true;
        return;
    }
    std::memcpy(peer_transport_params_.data(), params.data(), params.size());
    peer_transport_params_len_ = params.size();
}

void QuicTlsSession::on_quic_alert(tls::TlsAlertDesc alert) noexcept { record_alert(static_cast<std::uint8_t>(alert)); }

// =====================================================================
// handshake drive
// =====================================================================

common::IoResult<void> QuicTlsSession::provide_crypto_data(QuicEncryptionLevel level, const std::uint8_t *data,
                                                           std::size_t len) noexcept {
    if (!initialized() || (data == nullptr && len != 0)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    const tls::TlsQuicLevel tls_level = tls_level_from_encryption(level);
    // The done-transition releases the engine, so result_taken_ answers first.
    const bool done = result_taken_ || (client_mode_ ? client().done() : server().done());
    if (done) {
        // Post-handshake tail: the engine is terminal, app-level CRYPTO
        // belongs to the consumer (10 §7). The gate always runs
        // drive_handshake between provides, so the done-transition (which
        // appends the engine's own leftover first) has already run.
        if (len != 0) {
            if (post_buf_.size() + len > kMaxPostHandshakeBytes) {
                record_alert(static_cast<std::uint8_t>(tls::TlsAlertDesc::DecodeError));
                if (auto alert = take_pending_alert()) {
                    connection_->close_crypto_error(*alert);
                }
                return std::unexpected(common::IoErr::MessageTooLarge);
            }
            post_buf_.insert(post_buf_.end(), data, data + len);
        }
        return {};
    }
    // RFC 9001 §4.1.3: the engine consumes CRYPTO data level by level and
    // never returns to a lower one. A conforming peer cannot produce new
    // bytes below the highest level already fed -- each level's keys follow
    // from the data before it -- so they come from a misbehaving peer (e.g.
    // a 1-RTT CRYPTO frame ahead of the client Finished): unexpected_message.
    if (static_cast<std::uint8_t>(tls_level) < static_cast<std::uint8_t>(provided_level_)) {
        if (len == 0) {
            return {};
        }
        record_alert(static_cast<std::uint8_t>(tls::TlsAlertDesc::UnexpectedMessage));
        if (auto alert = take_pending_alert(); alert.has_value() && connection_ != nullptr) {
            connection_->close_crypto_error(*alert);
        }
        return std::unexpected(common::IoErr::Invalid);
    }
    provided_level_ = tls_level;
    if (client_mode_) {
        auto fed = client().feed_quic(tls_level, {data, len});
        if (!fed) {
            return std::unexpected(fed.error());
        }
    } else {
        auto fed = server().feed_quic(tls_level, {data, len});
        if (!fed) {
            return std::unexpected(fed.error());
        }
    }
    return {};
}

common::IoResult<void> QuicTlsSession::drive_handshake() noexcept {
    if (!initialized()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (result_taken_) {
        return process_post_handshake(); // the engine is released past the done-transition
    }
    const bool failed = client_mode_ ? client().failed() : server().failed();
    if (failed) {
        return fail_terminal();
    }
    const bool done = client_mode_ ? client().done() : server().done();
    if (done) {
        auto finished = finish_handshake();
        if (!finished) {
            return finished;
        }
        return process_post_handshake();
    }
    auto applied = apply_peer_transport_params();
    if (!applied && applied.error() != common::IoErr::WouldBlock) {
        return std::unexpected(applied.error());
    }
    return std::unexpected(common::IoErr::WouldBlock);
}

common::IoResult<void> QuicTlsSession::finish_handshake() noexcept {
    // The single handoff point (10 §8): terminal result, engine tail, peer
    // transport parameters, then the client's early-data verdict — all before
    // the caller marks the connection Established.
    result_taken_ = true;
    tls::TlsQuicHandshakeResult result = client_mode_ ? client().take_quic_result() : server().take_quic_result();
    resumption_master_ = std::move(result.resumption_master);
    alpn_ = result.alpn;
    alpn_len_ = result.alpn_len;
    session_resumed_ = result.session_resumed;
    early_data_accepted_ = result.early_data_accepted;
    take_engine_leftover();
    // The engine is spent: the server's NST went out with the final flight
    // and post-handshake CRYPTO belongs to the consumer. Release it — and a
    // credential retained for it — rather than pinning ~64-72 KiB for the
    // connection's lifetime. This runs on the connection's loop, as the
    // engine's node-pool affinity requires.
    delete client_;
    delete server_;
    client_ = nullptr;
    server_ = nullptr;
    credential_owner_.reset();

    auto applied = apply_peer_transport_params();
    if (!applied && applied.error() != common::IoErr::WouldBlock) {
        return std::unexpected(applied.error());
    }
    if (connection_ == nullptr || !connection_->peer_transport_params_received()) {
        // RFC 9001 §8.2: the handshake cannot complete without the peer's
        // transport parameters.
        if (connection_ != nullptr) {
            connection_->close(QuicErrorCode::TransportParameterError);
        }
        return std::unexpected(common::IoErr::Invalid);
    }
    if (client_mode_ && connection_->early_data_attempted()) {
        if (early_data_accepted_) {
            auto accepted = connection_->on_early_data_accepted();
            if (!accepted) {
                connection_->close(QuicErrorCode::TransportParameterError);
                return std::unexpected(accepted.error());
            }
        } else {
            // 0-RTT was refused (PSK rejected, or accepted without early
            // data). The engine already continued down the 1-RTT path (10
            // 定谳 5: no rollback, no re-drive); the QUIC layer just drops
            // the early state before anything 1-RTT observes it.
            connection_->on_early_data_rejected();
        }
    }
    return {};
}

common::IoResult<void> QuicTlsSession::fail_terminal() noexcept {
    credential_owner_.reset(); // a failed engine reads no more material
    const tls::TlsAlertDesc alert = client_mode_ && client_ != nullptr ? client().failure_alert()
                                    : server_ != nullptr               ? server().failure_alert()
                                                                       : tls::TlsAlertDesc::InternalError;
    record_alert(static_cast<std::uint8_t>(alert));
    common::IoErr error = common::IoErr::Invalid;
    switch (alert) { // certificate family = verification failure (10 §8: no X509 long codes)
        case tls::TlsAlertDesc::BadCertificate:
        case tls::TlsAlertDesc::UnsupportedCertificate:
        case tls::TlsAlertDesc::CertificateRevoked:
        case tls::TlsAlertDesc::CertificateExpired:
        case tls::TlsAlertDesc::CertificateUnknown:
        case tls::TlsAlertDesc::UnknownCa:
        case tls::TlsAlertDesc::AccessDenied:
            verify_failed_ = true;
            error = common::IoErr::Permission;
            break;
        default:
            break;
    }
    // The server's configure callback latches its own error — report that in
    // place of the generic failure, without a crypto close (the callback
    // already decided how the connection dies).
    if (!client_mode_ && callback_error_ != common::IoErr::None) {
        return std::unexpected(callback_error_);
    }
    // RFC 9000 §20.1: communicate the alert as a CONNECTION_CLOSE carrying
    // CRYPTO_ERROR = 0x0100 | alert (the mapping already specializes
    // no_application_protocol=120 and missing_extension=109).
    if (auto pending = take_pending_alert(); pending.has_value() && connection_ != nullptr) {
        connection_->close_crypto_error(*pending);
    }
    return std::unexpected(error);
}

common::IoResult<void> QuicTlsSession::apply_peer_transport_params() noexcept {
    if (connection_ == nullptr) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (connection_->peer_transport_params_received()) {
        return {};
    }
    if (peer_transport_params_overflow_ || peer_transport_params_len_ == 0) {
        if (peer_transport_params_overflow_) {
            connection_->close(QuicErrorCode::TransportParameterError);
            return std::unexpected(common::IoErr::Invalid);
        }
        return std::unexpected(common::IoErr::WouldBlock);
    }
    const QuicTransportParamOwner owner = connection_->role() == QuicConnectionRole::Client
                                                  ? QuicTransportParamOwner::Server
                                                  : QuicTransportParamOwner::Client;
    QuicTransportParams params{};
    QuicReadCursor reader(peer_transport_params_.data(), peer_transport_params_len_);
    auto parsed = quic_parse_transport_params(owner, reader, params);
    if (!parsed) {
        connection_->close(QuicErrorCode::TransportParameterError);
        return std::unexpected(parsed.error());
    }
    auto applied = connection_->apply_peer_transport_params(params);
    if (!applied) {
        connection_->close(QuicErrorCode::TransportParameterError);
        return std::unexpected(applied.error());
    }
    return {};
}

void QuicTlsSession::take_engine_leftover() noexcept {
    mem::IoBufChain leftover = client_mode_ ? client().take_inbound_leftover() : server().take_inbound_leftover();
    while (mem::IoBuf *node = leftover.first_readable()) {
        const std::span<const std::uint8_t> bytes{node->readable_data(), node->readable()};
        if (post_buf_.size() + bytes.size() <= kMaxPostHandshakeBytes) {
            post_buf_.insert(post_buf_.end(), bytes.begin(), bytes.end());
        }
        leftover.consume(bytes.size());
    }
}

// =====================================================================
// post-handshake consumer (10 §7)
// =====================================================================

common::IoResult<void> QuicTlsSession::process_post_handshake() noexcept {
    if (!result_taken_) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return pump_post_handshake();
}

common::IoResult<void> QuicTlsSession::pump_post_handshake() noexcept {
    const auto fatal = [this](std::uint8_t alert) noexcept {
        record_alert(alert);
        if (auto pending = take_pending_alert(); pending.has_value() && connection_ != nullptr) {
            connection_->close_crypto_error(*pending);
        }
        return std::unexpected(common::IoErr::Invalid);
    };
    std::size_t off = 0;
    while (post_buf_.size() - off >= kTlsHandshakeHeaderSize) {
        const std::uint8_t type = post_buf_[off];
        const std::size_t body_len = (static_cast<std::size_t>(post_buf_[off + 1]) << 16) |
                                     (static_cast<std::size_t>(post_buf_[off + 2]) << 8) |
                                     static_cast<std::size_t>(post_buf_[off + 3]);
        if (body_len > kMaxPostHandshakeBytes) {
            return fatal(static_cast<std::uint8_t>(tls::TlsAlertDesc::DecodeError));
        }
        if (post_buf_.size() - off - kTlsHandshakeHeaderSize < body_len) {
            break; // partial message — wait for the rest
        }
        const std::span<const std::uint8_t> body{post_buf_.data() + off + kTlsHandshakeHeaderSize, body_len};
        switch (static_cast<tls::TlsHandshakeType>(type)) {
            case tls::TlsHandshakeType::NewSessionTicket:
                if (!client_mode_) {
                    return fatal(static_cast<std::uint8_t>(tls::TlsAlertDesc::UnexpectedMessage));
                }
                handle_new_session_ticket(body);
                break;
            case tls::TlsHandshakeType::KeyUpdate:
                // RFC 9001 §6: key update is packet-layer only (the key_phase
                // bit); the TLS message is prohibited on QUIC connections —
                // fatal, exactly like BoringSSL's dispatcher (10 定谳 4).
                return fatal(static_cast<std::uint8_t>(tls::TlsAlertDesc::UnexpectedMessage));
            default:
                return fatal(static_cast<std::uint8_t>(tls::TlsAlertDesc::UnexpectedMessage));
        }
        off += kTlsHandshakeHeaderSize + body_len;
    }
    if (off != 0) {
        post_buf_.erase(post_buf_.begin(), post_buf_.begin() + static_cast<std::ptrdiff_t>(off));
    }
    return {};
}

void QuicTlsSession::handle_new_session_ticket(std::span<const std::uint8_t> body) noexcept {
    NstDecoded nst{};
    if (!decode_new_session_ticket(body, nst)) {
        record_alert(static_cast<std::uint8_t>(tls::TlsAlertDesc::DecodeError));
        if (auto pending = take_pending_alert(); pending.has_value() && connection_ != nullptr) {
            connection_->close_crypto_error(*pending);
        }
        return;
    }
    // The receipt (10 §6.2): PSK = resumption_master + ticket_nonce
    // (tls13_resumption_psk), everything else straight off the wire plus the
    // connection's own handshake facts.
    auto psk = tls13_resumption_psk(resumption_master_, nst.nonce);
    if (!psk) {
        return; // allocation failure: no receipt this ticket, connection unaffected
    }
    tls::TlsSessionState state{};
    state.identity.assign(nst.ticket.begin(), nst.ticket.end());
    state.psk = std::move(psk).value();
    state.suite = negotiated_suite_;
    state.ticket_age_add = nst.ticket_age_add;
    state.ticket_lifetime_s = nst.lifetime_s;
    state.max_early_data = nst.max_early_data;
    state.alpn = alpn_;
    state.alpn_len = alpn_len_;
    state.issued_ms = system_now_unix_ms();
    (void) connection_->on_new_tls_session(std::move(state)); // store-and-return contract
}

// =====================================================================
// accessors
// =====================================================================

// An engine is live until the done-transition releases it.
bool QuicTlsSession::initialized() const noexcept { return client_ != nullptr || server_ != nullptr || result_taken_; }

bool QuicTlsSession::handshake_done() const noexcept { return result_taken_; }

bool QuicTlsSession::session_reused() const noexcept { return result_taken_ && session_resumed_; }

std::string_view QuicTlsSession::selected_alpn() const noexcept {
    return {reinterpret_cast<const char *>(alpn_.data()), alpn_len_};
}

long QuicTlsSession::peer_verify_result() const noexcept {
    // No X509 long codes survive the engine swap (10 §8) — a nonzero value
    // still marks certificate verification as the failure reason.
    return verify_failed_ && last_alert_.has_value() ? static_cast<long>(*last_alert_) : 0;
}

void QuicTlsSession::record_alert(std::uint8_t alert) noexcept {
    pending_alert_ = alert;
    last_alert_ = alert;
}

std::optional<std::uint8_t> QuicTlsSession::take_pending_alert() noexcept {
    std::optional<std::uint8_t> alert = pending_alert_;
    pending_alert_.reset();
    return alert;
}

tls::TlsClientHandshakeEngine &QuicTlsSession::client() noexcept {
    FIBER_ASSERT(client_ != nullptr);
    return *client_;
}

tls::TlsServerHandshakeEngine &QuicTlsSession::server() noexcept {
    FIBER_ASSERT(server_ != nullptr);
    return *server_;
}

const tls::TlsClientHandshakeEngine &QuicTlsSession::client() const noexcept {
    FIBER_ASSERT(client_ != nullptr);
    return *client_;
}

const tls::TlsServerHandshakeEngine &QuicTlsSession::server() const noexcept {
    FIBER_ASSERT(server_ != nullptr);
    return *server_;
}

} // namespace fiber::quic
