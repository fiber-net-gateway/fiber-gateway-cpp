#include "Tls12ServerHandshake.h"

#include <cstring>
#include <utility>

#include "../crypto/TlsCryptoPrimitives.h"

#include <fiber/tls/TlsVersion.h>
#include <fiber/tls/crypto/TlsCertificate.h>
#include <fiber/tls/crypto/TlsSignature.h>
#include <fiber/tls/record/TlsRecord.h>

namespace fiber::tls {

Tls12ServerHandshake::Tls12ServerHandshake(const Mount &mount) noexcept :
    ctx_(mount.ctx), cfg_(mount.cfg), resumption_(mount.resumption), minter_(mount.minter), hello_(mount.hello),
    out_(mount.out), scratch_(mount.scratch) {}

Tls12ServerHandshake::~Tls12ServerHandshake() {
    // Explicit wipes at the handoff points the 02 contract names; the key
    // exchange wipes itself. master12_ already moved into state_ arrives here
    // moved-from (pre-wiped).
    master12_.wipe();
    wipe_kb12();
    tls_secure_wipe(z12_.z.data(), z12_.z.size());
}

// =====================================================================
// Entry: ClientHello (1.2 fork)
// =====================================================================

void Tls12ServerHandshake::start(const TlsClientHello &ch, std::span<const std::uint8_t> body) noexcept {
    // ServerHello.random = gmt_unix_time(4) + random(28) per RFC 5246, with
    // the RFC 8446 §4.1.3 downgrade sentinel in the last 8 bytes when the
    // ClientHello does not offer 1.3 — that is the exact §4.1.3 send
    // condition (a CH whose supported_versions may have been stripped). A
    // genuine 1.2-only client never checks the sentinel; a 1.3-capable
    // client that receives it aborts, which is the point.
    if (!tls_random_bytes(hello_.server_random)) {
        fail(TlsAlertDesc::InternalError);
        return;
    }
    const std::uint32_t gmt = static_cast<std::uint32_t>(cfg_.now_unix_ms / 1000);
    hello_.server_random[0] = static_cast<std::uint8_t>(gmt >> 24);
    hello_.server_random[1] = static_cast<std::uint8_t>(gmt >> 16);
    hello_.server_random[2] = static_cast<std::uint8_t>(gmt >> 8);
    hello_.server_random[3] = static_cast<std::uint8_t>(gmt);
    const bool client_offers13 =
            ch.has_supported_versions && tls_server_list_contains(ch.supported_versions, kTlsVersionTls13);
    if (!client_offers13) {
        static constexpr std::array<std::uint8_t, 8> kDowngradeSentinel{0x44, 0x4F, 0x57, 0x4E, 0x47, 0x52, 0x44, 0x01};
        std::memcpy(hello_.server_random.data() + 24, kDowngradeSentinel.data(), kDowngradeSentinel.size());
    }

    // The abbreviated shape first (08): a presented ticket the lookup
    // accepts. Every miss falls through to the full handshake below — a
    // rejected ticket is never a connection failure.
    if (try_resume_12(ch)) {
        send_abbreviated_flight_12(ch, body);
        return;
    }

    // Suite: server preference × client offer × the credential's key kind
    // (an RSA key drives ECDHE-RSA, a P-256/384 key ECDHE-ECDSA).
    if (!tls_server_suite_select_12(ch, *cfg_.key, suite_)) {
        fail(TlsAlertDesc::HandshakeFailure);
        return;
    }
    // Group: server preference walk (the 1.2 CH carries no key_share — the
    // curve arrives here; a fresh ephemeral pair is generated for the SKE).
    if (!tls_server_group_select(ch, hello_.kx_group)) {
        fail(TlsAlertDesc::HandshakeFailure);
        return;
    }
    auto kx = TlsKeyExchange::create(hello_.kx_group);
    if (!kx.has_value() || !(*kx)->generate().has_value()) {
        fail(TlsAlertDesc::InternalError);
        return;
    }
    hello_.kx = std::move(*kx);

    ems_negotiated_ = ch.has_extended_master_secret;
    // EMS rides the gate (08): tickets only ever mint for EMS sessions, so a
    // non-EMS CH gets neither the SH echo nor an NST — no resumption offers
    // across the RFC 7627 boundary can exist.
    ticket_wanted_ = minter_ != nullptr && ch.has_session_ticket && ems_negotiated_;

    // ALPN: server preference walk; 1.2 carries the selection in the SH (the
    // 1.3 EE role), same outcome triple.
    std::string_view alpn;
    const TlsServerAlpnResult alpn_result = tls_server_alpn_select(cfg_, ch, alpn);
    if (alpn_result == TlsServerAlpnResult::Failed) {
        fail(TlsAlertDesc::NoApplicationProtocol);
        return;
    }
    if (alpn_result == TlsServerAlpnResult::Matched) {
        FIBER_ASSERT(alpn.size() <= state_.alpn.size());
        std::memcpy(state_.alpn.data(), alpn.data(), alpn.size());
        state_.alpn_len = static_cast<std::uint16_t>(alpn.size());
    }

    // The client flight stays plaintext until its CCS.
    ctx_.set_inbound_mode(TlsInboundMode::Plaintext12);

    if (!t12_.init(suite_info()->hash)) {
        fail(TlsAlertDesc::InternalError);
        return;
    }
    feed12(TlsHandshakeType::ClientHello, body);

    send_server_flight_12(ch);
}

// =====================================================================
// Abbreviated acceptance (08): RFC 5077 ticket + RFC 5246 §7.3 resumed
// shape. The lookup (stateless open: AAD name binding, tamper, expiry)
// plus the local gates — each miss is a full-handshake fallback.
// =====================================================================

bool Tls12ServerHandshake::try_resume_12(const TlsClientHello &ch) noexcept {
    // Structural preconditions: a presented ticket, a sid worth echoing (the
    // client detects resumption by the echo — an empty sid cannot carry it),
    // a lookup, and no mTLS (a resumed connection carries no client
    // certificate; a client_trust config needs the real flight).
    if (resumption_ == nullptr || resumption_->lookup == nullptr || cfg_.client_trust != nullptr ||
        !ch.has_session_ticket || ch.session_ticket.empty() || ch.session_id.empty()) {
        return false;
    }
    TlsResumedSession resumed{};
    if (!resumption_->lookup(resumption_->ctx, ch.session_ticket, hello_.view.server_name, cfg_.now_unix_ms, resumed)) {
        return false;
    }
    // The lookup is version-blind — only a 1.2-payload ticket resumes here.
    if (resumed.version != TlsProtocolVersion::Tls12) {
        return false;
    }
    // Suite: a 1.2 registry suite the CH still offers (the credential never
    // signs on a resumed connection — the auth half of the name is moot).
    const TlsSuiteInfo *info = tls_suite_info(resumed.suite);
    if (info == nullptr || info->is_tls13 ||
        !tls_server_list_contains(ch.cipher_suites, static_cast<std::uint16_t>(resumed.suite))) {
        return false;
    }
    // EMS (RFC 7627 §5.3): tickets only exist for EMS sessions, and the
    // resumption CH must still offer it — no cross-boundary resumption.
    if (!ch.has_extended_master_secret) {
        return false;
    }
    // ALPN binding: the ticket's protocol survives only while the client
    // keeps offering it; a ticket without ALPN resumes without one.
    if (!resumed.alpn.empty() && !tls_ch_offers_alpn(ch, resumed.alpn)) {
        return false;
    }

    // Accept: copy the borrowed material immediately (the lookup's staging
    // cell is single-shot) and reuse the master for this connection's keys.
    master12_ = TlsSecret::from_bytes(resumed.psk); // the 48-byte master
    if (master12_.empty()) {
        return false; // empty secret = malformed payload; degrade, don't fail
    }
    auto keys = tls12_key_block(resumed.suite, master12_, ch.random, hello_.server_random);
    if (!keys.has_value()) {
        master12_.wipe();
        return false;
    }
    suite_ = resumed.suite;
    kb12_ = keys.value();
    ems_negotiated_ = true; // the SH echoes what the minted session used
    ticket_wanted_ = minter_ != nullptr; // presenting the ticket IS a 5077 offer
    if (!resumed.alpn.empty()) {
        FIBER_ASSERT(resumed.alpn.size() <= state_.alpn.size());
        std::memcpy(state_.alpn.data(), resumed.alpn.data(), resumed.alpn.size());
        state_.alpn_len = static_cast<std::uint16_t>(resumed.alpn.size());
    }
    resumed12_ = true;
    return true;
}

// SH [NST] CCS Fin — the abbreviated server flight (RFC 5246 §7.3): the
// ticket's suite and the reused master, the CH's sid echoed, a rotation NST
// when a minter is wired, then the CCS + the server Finished FIRST (the
// 1.3-style order — the client's Fin MACs ours in).
void Tls12ServerHandshake::send_abbreviated_flight_12(const TlsClientHello &ch,
                                                      std::span<const std::uint8_t> body) noexcept {
    // The client's CCS + Finished arrive after our flight.
    ctx_.set_inbound_mode(TlsInboundMode::Plaintext12);
    if (!t12_.init(suite_info()->hash)) {
        fail(TlsAlertDesc::InternalError);
        return;
    }
    feed12(TlsHandshakeType::ClientHello, body);

    // ---- ServerHello: the sid echo IS the resumption signal ----
    TlsServerHelloInput sh{};
    sh.random = hello_.server_random;
    sh.session_id = ch.session_id;
    sh.cipher_suite = static_cast<std::uint16_t>(suite_);
    sh.tls13 = false;
    sh.extended_master_secret = true; // the minted session's EMS, re-echoed
    sh.renegotiation_info = true;
    if (state_.alpn_len > 0) {
        sh.alpn = std::string_view{reinterpret_cast<const char *>(state_.alpn.data()), state_.alpn_len};
    }
    sh.session_ticket = ticket_wanted_; // empty-payload echo; the NST follows
    const auto sh_len = tls_encode_server_hello(sh, scratch_);
    if (!sh_len.has_value() || !t12_.update({scratch_.data(), sh_len.value()}) ||
        !emit_message({scratch_.data(), sh_len.value()})) {
        fail(TlsAlertDesc::InternalError);
        return;
    }

    // ---- NewSessionTicket (rotation, plaintext, before the CCS) ----
    // The re-sealed master rides a fresh nonce; the server Fin MACs it in.
    if (ticket_wanted_) {
        TlsTicketRequest req{};
        req.resumption_master = master12_.bytes(); // 1.2's resumption secret IS the master
        req.ticket_nonce = 0;
        req.suite = suite_;
        req.alpn = state_.alpn_len > 0
                           ? std::string_view{reinterpret_cast<const char *>(state_.alpn.data()), state_.alpn_len}
                           : std::string_view{};
        req.max_early_data = 0; // 0-RTT is a 1.3-only property
        req.timeout_s = cfg_.session_timeout_s;
        req.now_unix_ms = cfg_.now_unix_ms;
        req.version = TlsProtocolVersion::Tls12;
        req.name = hello_.view.server_name; // the CH's SNI over the retained copy — stable
        const std::size_t ticket_len = minter_->mint(minter_->ctx, req, ticket_buf_);
        if (ticket_len > 0 && ticket_len <= ticket_buf_.size()) {
            const auto nst_len = tls_encode_new_session_ticket_12(cfg_.session_timeout_s,
                                                                  {ticket_buf_.data(), ticket_len}, scratch_);
            if (!nst_len.has_value() || !t12_.update({scratch_.data(), nst_len.value()}) ||
                !emit_message({scratch_.data(), nst_len.value()})) {
                fail(TlsAlertDesc::InternalError);
                return;
            }
        }
        // mint 0 = no rotation this connection; the SH echo already promised
        // nothing the client cannot survive (it keeps the old ticket).
    }

    // ---- CCS + write-cipher swap ----
    if (!ctx_.send_ccs().has_value() || !swap_cipher_12(ctx_.write_cipher(), kb12_.server)) {
        fail(TlsAlertDesc::InternalError);
        return;
    }

    // ---- server Finished (sealed; MAC over CH‖SH‖NST — the client Fin,
    // still to come, MACs this message in too) ----
    snapshot12();
    auto verify = tls12_verify_data(suite_, master12_, false, {hash_buf_.data(), hash_len()});
    if (!verify.has_value()) {
        fail(TlsAlertDesc::InternalError);
        return;
    }
    const auto fin_len = tls_encode_finished({verify->data(), verify->size()}, scratch_);
    if (!fin_len.has_value() || !t12_.update({scratch_.data(), fin_len.value()}) ||
        !emit_message({scratch_.data(), fin_len.value()})) {
        fail(TlsAlertDesc::InternalError);
        return;
    }
    st_ = St::ExpectClientCcs12;
}

// =====================================================================
// Server flight: SH Cert SKE [CertReq] SHD (RFC 5246 §7.3 order — the
// CertificateRequest follows ServerKeyExchange, immediately before SHD)
// =====================================================================

void Tls12ServerHandshake::send_server_flight_12(const TlsClientHello &ch) noexcept {
    // ---- ServerHello ----
    TlsServerHelloInput sh{};
    sh.random = hello_.server_random;
    // An EMPTY sid: echoing the CH's non-empty sid IS the resumption signal
    // (RFC 5246 §7.4.1.3) — a client holding a session with that sid would
    // expect the abbreviated flight and reject the Certificate that follows.
    // This server is stateless (no id cache), so the full shape never
    // resumes by id. The abbreviated shape is the one legal echo.
    sh.session_id = std::span<const std::uint8_t>{};
    sh.cipher_suite = static_cast<std::uint16_t>(suite_);
    sh.tls13 = false;
    sh.extended_master_secret = ems_negotiated_;
    sh.renegotiation_info = true; // empty RI, always (RFC 5746 §3.4 server answer)
    if (state_.alpn_len > 0) {
        sh.alpn = std::string_view{reinterpret_cast<const char *>(state_.alpn.data()), state_.alpn_len};
    }
    sh.session_ticket = ticket_wanted_; // empty-payload echo; the NST follows the client Fin
    const auto sh_len = tls_encode_server_hello(sh, scratch_);
    if (!sh_len.has_value() || !t12_.update({scratch_.data(), sh_len.value()}) ||
        !emit_message({scratch_.data(), sh_len.value()})) {
        fail(TlsAlertDesc::InternalError);
        return;
    }

    // ---- Certificate (1.2 chain form: leaf + intermediates, no context) ----
    const TlsCertificateChain &chain = *cfg_.chain;
    std::array<std::span<const std::uint8_t>, TlsCertificateChain::kMaxCerts> ders{};
    ders[0] = chain.leaf().der();
    for (std::size_t i = 0; i < chain.intermediates().size(); ++i) {
        ders[i + 1] = chain.intermediates()[i].der();
    }
    const auto cert_len = tls_encode_certificate_12({ders.data(), chain.size()}, scratch_);
    if (!cert_len.has_value() || !t12_.update({scratch_.data(), cert_len.value()}) ||
        !emit_message({scratch_.data(), cert_len.value()})) {
        fail(TlsAlertDesc::InternalError);
        return;
    }

    // ---- ServerKeyExchange ----
    // The SKE signature scheme: first 1.2 preference entry the credential
    // supports that the CH offered.
    TlsSignatureScheme scheme = TlsSignatureScheme::RsaPkcs1Sha256;
    if (!tls_server_cv_scheme_select(*cfg_.key, ch, TlsProtocolVersion::Tls12, scheme)) {
        fail(TlsAlertDesc::HandshakeFailure); // no common signature scheme
        return;
    }
    // The signature covers the 1.2 "ServerDHParams" prefixed with both
    // randoms — client_random || server_random || curve_type(3) || be16(group)
    // || u8(point_len) || point (the same bytes the 06 client verifies; NOT a
    // transcript digest).
    static constexpr std::size_t kSkeContentMax = 64 + 1 + 2 + 1 + 255;
    std::array<std::uint8_t, kSkeContentMax> content{};
    std::memcpy(content.data(), ch.random.data(), 32);
    std::memcpy(content.data() + 32, hello_.server_random.data(), 32);
    std::size_t off = 64;
    const std::uint16_t group_raw = static_cast<std::uint16_t>(hello_.kx_group);
    const TlsKeySharePub &point = hello_.kx->public_value();
    content[off++] = 0x03; // curve_type named_curve
    content[off++] = static_cast<std::uint8_t>(group_raw >> 8);
    content[off++] = static_cast<std::uint8_t>(group_raw);
    content[off++] = static_cast<std::uint8_t>(point.len);
    std::memcpy(content.data() + off, point.bytes().data(), point.len);
    off += point.len;

    std::array<std::uint8_t, kServerMaxSigLen> sig{};
    FIBER_ASSERT(cfg_.key->max_signature_len() <= sig.size());
    const auto sig_len = cfg_.key->sign(scheme, TlsProtocolVersion::Tls12, {content.data(), off}, sig);
    if (!sig_len.has_value()) {
        fail(TlsAlertDesc::InternalError);
        return;
    }
    TlsServerKeyExchangeInput ske{};
    ske.named_group = group_raw;
    ske.public_key = point.bytes();
    ske.scheme = static_cast<std::uint16_t>(scheme);
    ske.signature = {sig.data(), sig_len.value()};
    const auto ske_len = tls_encode_server_key_exchange(ske, scratch_);
    if (!ske_len.has_value() || !t12_.update({scratch_.data(), ske_len.value()}) ||
        !emit_message({scratch_.data(), ske_len.value()})) {
        fail(TlsAlertDesc::InternalError);
        return;
    }

    // ---- CertificateRequest (mTLS) ----
    if (cfg_.client_trust != nullptr) {
        const auto cr_len = tls_encode_certificate_request_12(kServerCr12Sigalgs, scratch_);
        if (!cr_len.has_value() || !t12_.update({scratch_.data(), cr_len.value()}) ||
            !emit_message({scratch_.data(), cr_len.value()})) {
            fail(TlsAlertDesc::InternalError);
            return;
        }
    }

    // ---- ServerHelloDone (empty) ----
    const auto shd_len = tls_encode_handshake_message(TlsHandshakeType::ServerHelloDone, {}, scratch_);
    if (!shd_len.has_value() || !t12_.update({scratch_.data(), shd_len.value()}) ||
        !emit_message({scratch_.data(), shd_len.value()})) {
        fail(TlsAlertDesc::InternalError);
        return;
    }

    st_ = cfg_.client_trust != nullptr ? St::ExpectClientCert12 : St::ExpectCke12;
}

// =====================================================================
// Post-fork inbound traffic
// =====================================================================

void Tls12ServerHandshake::on_message(TlsHandshakeType type, std::span<const std::uint8_t> body) noexcept {
    switch (st_) {
        case St::ExpectClientCert12:
            if (type != TlsHandshakeType::Certificate) {
                fail(TlsAlertDesc::UnexpectedMessage);
                return;
            }
            handle_client_certificate_12(body);
            return;
        case St::ExpectCke12:
            if (type != TlsHandshakeType::ClientKeyExchange) {
                fail(TlsAlertDesc::UnexpectedMessage);
                return;
            }
            handle_client_key_exchange_12(body);
            return;
        case St::ExpectClientCv12:
            if (type != TlsHandshakeType::CertificateVerify) {
                fail(TlsAlertDesc::UnexpectedMessage);
                return;
            }
            handle_client_certificate_verify_12(body);
            return;
        case St::ExpectClientCcs12:
            // Nothing is owed between the client flight and its CCS.
            fail(TlsAlertDesc::UnexpectedMessage);
            return;
        case St::ExpectClientFin12:
            if (type != TlsHandshakeType::Finished) {
                fail(TlsAlertDesc::UnexpectedMessage);
                return;
            }
            handle_client_finished_12(body);
            return;
        case St::Done:
            FIBER_ASSERT(false); // the pump stops at done; a message here is an engine bug
            return;
    }
}

void Tls12ServerHandshake::on_ccs() noexcept {
    switch (st_) {
        case St::ExpectClientCcs12:
            // The client's one CCS switches the read side to the key_block
            // client keys; the client Fin is the first sealed record inbound.
            if (!swap_cipher_12(ctx_.read_cipher(), kb12_.client)) {
                fail(TlsAlertDesc::InternalError);
                return;
            }
            ctx_.set_inbound_mode(TlsInboundMode::Sealed12);
            st_ = St::ExpectClientFin12;
            break;
        case St::ExpectClientCert12:
        case St::ExpectCke12:
        case St::ExpectClientCv12:
            // The client's CCS belongs after its flight, right before its
            // Finished — earlier than that is a fault.
            fail(TlsAlertDesc::UnexpectedMessage);
            break;
        default:
            // A repeat CCS at/after the expected point is middlebox noise;
            // the context validated the 1-byte 0x01 form.
            break;
    }
}

// =====================================================================
// Client flight
// =====================================================================

void Tls12ServerHandshake::handle_client_certificate_12(std::span<const std::uint8_t> body) noexcept {
    TlsCertificate12 cert;
    if (!tls_decode_certificate_12(body.data(), body.size(), cert).has_value()) {
        fail(TlsAlertDesc::DecodeError);
        return;
    }
    if (cert.cert_count == 0) {
        // An empty chain is structurally valid (07 §3.2): fatal only when the
        // config requires a client certificate.
        if (cfg_.require_client_cert) {
            fail(TlsAlertDesc::CertificateRequired);
            return;
        }
        feed12(TlsHandshakeType::Certificate, body);
        st_ = St::ExpectCke12; // no credential — no CertificateVerify
        return;
    }
    auto chain = TlsCertificateChain::from_der_list({cert.certs, cert.cert_count});
    if (!chain.has_value()) {
        fail(TlsAlertDesc::DecodeError);
        return;
    }
    peer_chain_ = std::move(chain).value();
    peer_sent_cert12_ = true;
    feed12(TlsHandshakeType::Certificate, body);

    // Path validation with no name (a client chain has no SNI to match);
    // purpose SslClient, the config's clock and trust anchors.
    const auto verification =
            tls_verify_chain(peer_chain_, *cfg_.client_trust, TlsCertPurpose::SslClient, "", {}, cfg_.now_unix_ms);
    if (!verification.has_value()) {
        fail(TlsAlertDesc::InternalError);
        return;
    }
    if (verification->status == TlsCertVerification::Status::NotTrusted) {
        fail(verification->alert);
        return;
    }
    st_ = St::ExpectCke12;
}

void Tls12ServerHandshake::handle_client_key_exchange_12(std::span<const std::uint8_t> body) noexcept {
    TlsClientKeyExchange cke;
    if (!tls_decode_client_key_exchange(body.data(), body.size(), cke).has_value()) {
        fail(TlsAlertDesc::DecodeError);
        return;
    }
    const TlsKxShared z = hello_.kx->decap(cke.public_key);
    if (z.status == TlsKxStatus::BadPeerData) {
        fail(TlsAlertDesc::IllegalParameter); // length/point violations
        return;
    }
    if (z.status != TlsKxStatus::Ok) {
        fail(TlsAlertDesc::InternalError);
        return;
    }
    z12_ = z;
    feed12(TlsHandshakeType::ClientKeyExchange, body);

    // ---- master secret + key_block at the CKE point ----
    // The EMS session_hash is the LIVE transcript at derivation time — the
    // exact bytes the client snapshotted when it derived (BoringSSL
    // ssl_hash_current_message(CKE) precedes tls1_generate_master_secret on
    // both peers): everything through CKE, only the still-unwritten client CV
    // excluded.
    if (ems_negotiated_) {
        snapshot12();
        auto master = tls12_extended_master_secret(suite_, z12_.z, {hash_buf_.data(), hash_len()});
        if (!master.has_value()) {
            fail(TlsAlertDesc::InternalError);
            return;
        }
        master12_ = std::move(master).value();
    } else {
        auto master = tls12_master_secret(suite_, z12_.z, hello_.view.random, hello_.server_random);
        if (!master.has_value()) {
            fail(TlsAlertDesc::InternalError);
            return;
        }
        master12_ = std::move(master).value();
    }
    auto keys = tls12_key_block(suite_, master12_, hello_.view.random, hello_.server_random);
    if (!keys.has_value()) {
        fail(TlsAlertDesc::InternalError);
        return;
    }
    kb12_ = keys.value();

    st_ = peer_sent_cert12_ ? St::ExpectClientCv12 : St::ExpectClientCcs12;
}

void Tls12ServerHandshake::handle_client_certificate_verify_12(std::span<const std::uint8_t> body) noexcept {
    TlsCertificateVerify cv;
    if (!tls_decode_certificate_verify(body.data(), body.size(), cv).has_value()) {
        fail(TlsAlertDesc::DecodeError);
        return;
    }
    // The scheme must come from the CertificateRequest offer.
    bool offered = false;
    for (const std::uint16_t raw: kServerCr12Sigalgs) {
        if (raw == cv.algorithm) {
            offered = true;
            break;
        }
    }
    if (!offered) {
        fail(TlsAlertDesc::IllegalParameter);
        return;
    }
    // A 1.2 CV signs the RAW handshake_messages themselves (no pre-digested
    // form exists): the same t12_.buffer() bytes the client signed — the
    // transcript through its CKE, snapshotted BEFORE this message is fed.
    const auto public_key = peer_chain_.leaf().public_key();
    if (!public_key.has_value()) {
        fail(TlsAlertDesc::InternalError);
        return;
    }
    const auto valid = tls_verify(static_cast<TlsSignatureScheme>(cv.algorithm), TlsProtocolVersion::Tls12, *public_key,
                                  t12_.buffer(), cv.signature);
    if (!valid.has_value()) {
        fail(TlsAlertDesc::IllegalParameter); // scheme does not match the peer's key
        return;
    }
    if (!valid.value()) {
        fail(TlsAlertDesc::DecryptError);
        return;
    }
    feed12(TlsHandshakeType::CertificateVerify, body);
    st_ = St::ExpectClientCcs12;
}

void Tls12ServerHandshake::handle_client_finished_12(std::span<const std::uint8_t> body) noexcept {
    TlsFinished fin;
    if (!tls_decode_finished(body.data(), body.size(), fin).has_value() || fin.verify_data.size() != 12) {
        fail(TlsAlertDesc::DecodeError); // 1.2 verify_data is always 12 bytes
        return;
    }
    // verify_data MACs the message list through whatever preceded it (the
    // CCS is not a handshake message) — snapshotted before the client Fin is
    // fed. Full shape: CH..client flight; abbreviated: CH‖SH‖NST‖server Fin.
    snapshot12();
    const auto expected = tls12_verify_data(suite_, master12_, true, {hash_buf_.data(), hash_len()});
    if (!expected.has_value()) {
        fail(TlsAlertDesc::InternalError);
        return;
    }
    if (!tls_constant_time_equal({expected->data(), expected->size()}, fin.verify_data)) {
        fail(TlsAlertDesc::DecryptError);
        return;
    }
    feed12(TlsHandshakeType::Finished, body);
    if (resumed12_) {
        // The abbreviated flight already went out — the client Fin closes it.
        finish_1_2();
        return;
    }
    if (!send_final_flight_12()) {
        return; // the flight raised its own alert
    }
    finish_1_2();
}

// [NST] CCS Fin — every failure path raises its own alert. The client flight
// arrived sealed; the CCS swaps the write side, so the server Fin is the
// first sealed record outbound.
bool Tls12ServerHandshake::send_final_flight_12() noexcept {
    // ---- NewSessionTicket (plaintext, RFC 5077 §3.3) ----
    // Before the CCS and transcript-hashed, so the server Fin MAC covers it
    // (the 06 client feeds the NST the same way before verifying our Fin).
    if (ticket_wanted_) {
        TlsTicketRequest req{};
        req.resumption_master = master12_.bytes(); // 1.2's resumption secret IS the master
        req.ticket_nonce = 0;
        req.suite = suite_;
        req.alpn = state_.alpn_len > 0
                           ? std::string_view{reinterpret_cast<const char *>(state_.alpn.data()), state_.alpn_len}
                           : std::string_view{};
        req.max_early_data = 0; // 0-RTT is a 1.3-only property
        req.timeout_s = cfg_.session_timeout_s;
        req.now_unix_ms = cfg_.now_unix_ms;
        req.version = TlsProtocolVersion::Tls12;
        req.name = hello_.view.server_name; // the CH's SNI over the retained copy — stable
        const std::size_t ticket_len = minter_->mint(minter_->ctx, req, ticket_buf_);
        if (ticket_len > 0 && ticket_len <= ticket_buf_.size()) {
            const auto nst_len = tls_encode_new_session_ticket_12(cfg_.session_timeout_s,
                                                                  {ticket_buf_.data(), ticket_len}, scratch_);
            if (!nst_len.has_value() || !t12_.update({scratch_.data(), nst_len.value()}) ||
                !emit_message({scratch_.data(), nst_len.value()})) {
                fail(TlsAlertDesc::InternalError);
                return false;
            }
        }
        // A minter returning 0 (or an out-of-bounds length) simply means no
        // ticket this connection — the SH already echoed the extension, which
        // RFC 5077 permits (the client just never gets an NST).
    }

    // ---- CCS + write-cipher swap ----
    if (!ctx_.send_ccs().has_value() || !swap_cipher_12(ctx_.write_cipher(), kb12_.server)) {
        fail(TlsAlertDesc::InternalError);
        return false;
    }

    // ---- server Finished (sealed; MAC over the pre-Fin transcript, NST
    // included) ----
    snapshot12();
    auto verify = tls12_verify_data(suite_, master12_, false, {hash_buf_.data(), hash_len()});
    if (!verify.has_value()) {
        fail(TlsAlertDesc::InternalError);
        return false;
    }
    const auto fin_len = tls_encode_finished({verify->data(), verify->size()}, scratch_);
    if (!fin_len.has_value() || !t12_.update({scratch_.data(), fin_len.value()}) ||
        !emit_message({scratch_.data(), fin_len.value()})) {
        fail(TlsAlertDesc::InternalError);
        return false;
    }
    return true;
}

void Tls12ServerHandshake::finish_1_2() noexcept {
    state_.version = TlsProtocolVersion::Tls12;
    state_.suite = suite_;
    state_.read_cipher = std::move(ctx_.read_cipher());
    state_.write_cipher = std::move(ctx_.write_cipher());
    state_.tls12_master = std::move(master12_);
    state_.session_resumed = resumed12_;
    state_.early_data_accepted = false;
    state_.peer_chain = std::move(peer_chain_);
    wipe_kb12(); // both ciphers hold the key_block material now
    out_.done = true;
    st_ = St::Done;
    // Reader bytes after the server Fin stay buffered — post-handshake
    // messages belong to 08.
}

} // namespace fiber::tls
