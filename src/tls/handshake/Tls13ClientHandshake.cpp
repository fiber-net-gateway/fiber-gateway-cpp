#include "Tls13ClientHandshake.h"

#include <cstring>
#include <utility>

#include "../crypto/TlsCryptoPrimitives.h"

#include <fiber/tls/TlsVersion.h>
#include <fiber/tls/crypto/Tls13KeySchedule.h>
#include <fiber/tls/crypto/TlsCertificate.h>
#include <fiber/tls/crypto/TlsSignature.h>
#include <fiber/tls/handshake/TlsExtensionCodec.h>
#include <fiber/tls/record/TlsRecord.h>

namespace fiber::tls {

namespace {

constexpr std::size_t kServerCvCtxLen = sizeof("TLS 1.3, server CertificateVerify") - 1;
constexpr std::size_t kClientCvCtxLen = sizeof("TLS 1.3, client CertificateVerify") - 1;
constexpr std::size_t kCvContentMax = 64 + kClientCvCtxLen + 1 + 48; // 64 spaces + ctx + NUL + SHA-384

// CertificateVerify content (RFC 8446 §4.4.3): 64×0x20, the context string, a
// zero byte, then the transcript snapshot. The context string and the snapshot
// point differ per signer; the shape is shared.
void build_cert_verify_content(std::span<std::uint8_t> out, std::span<const char> context,
                               std::span<const std::uint8_t> transcript_hash) noexcept {
    FIBER_ASSERT(out.size() == 64 + context.size() + 1 + transcript_hash.size());
    std::memset(out.data(), 0x20, 64);
    std::memcpy(out.data() + 64, context.data(), context.size());
    out[64 + context.size()] = 0x00;
    std::memcpy(out.data() + 64 + context.size() + 1, transcript_hash.data(), transcript_hash.size());
}

} // namespace

Tls13ClientHandshake::Tls13ClientHandshake(const Mount &mount) noexcept :
    ctx_(mount.ctx), cfg_(mount.cfg), session_(mount.session), hello_(mount.hello), sched_(mount.sched),
    early_(mount.early), out_(mount.out), scratch_(mount.scratch), psk_offered_(mount.psk_offered),
    ccs_sent_(mount.ccs_sent) {}

Tls13ClientHandshake::~Tls13ClientHandshake() {
    // Explicit wipes at the handoff points the 02 contract names; the
    // schedule and key exchange wipe themselves. Secrets already moved into
    // state_ arrive here moved-from (pre-wiped).
    client_hs_.wipe();
    server_hs_.wipe();
    client_app0_.wipe();
    server_app0_.wipe();
    resumption_master_.wipe();
}

// =====================================================================
// Entry: ServerHello / HelloRetryRequest
// =====================================================================

void Tls13ClientHandshake::start(const TlsServerHello &sh, std::span<const std::uint8_t> body) noexcept {
    if (sh.compression_method != 0 || !sh.has_key_share || sh.key_share.empty() || sh.has_cookie) {
        // A real SH must carry the server's share (the empty selected_group
        // form belongs to HRR); cookie is HRR-only.
        fail(sh.has_cookie ? TlsAlertDesc::UnsupportedExtension : TlsAlertDesc::IllegalParameter);
        return;
    }
    if (sh.has_alpn || sh.has_extended_master_secret || sh.has_renegotiation_info) {
        fail(TlsAlertDesc::UnsupportedExtension); // 1.2-only echoes never appear in a 1.3 SH
        return;
    }
    if (tls_client_suite_offer_index(sh.cipher_suite) == kOfferedSuites.size()) {
        fail(TlsAlertDesc::IllegalParameter);
        return;
    }
    suite_ = static_cast<TlsCipherSuiteId>(sh.cipher_suite);
    const TlsSuiteInfo *info = suite_info();
    if (info == nullptr || !info->is_tls13) {
        fail(TlsAlertDesc::IllegalParameter);
        return;
    }
    if (hrr_count_ == 1 && suite_ != hrr_suite_) {
        fail(TlsAlertDesc::IllegalParameter); // transcript-hash stability (06 §2.1)
        return;
    }
    if (sh.session_id.size() != hello_.session_id.size() ||
        std::memcmp(sh.session_id.data(), hello_.session_id.data(), hello_.session_id.size()) != 0) {
        fail(TlsAlertDesc::IllegalParameter);
        return;
    }
    if (sh.key_share_group != static_cast<std::uint16_t>(hello_.kx_group)) {
        fail(TlsAlertDesc::IllegalParameter);
        return;
    }

    // PSK outcome (06 §2.5): accepted (index 0 of our single offer, suite
    // matching the binder's hash) or rejected (fresh no-PSK schedule). Either
    // way the early-data window closes at the SH read point.
    if (sh.has_selected_identity) {
        if (!psk_offered_ || sh.selected_identity != 0 || suite_ != session_->suite) {
            fail(TlsAlertDesc::IllegalParameter);
            return;
        }
        psk_accepted_ = true;
    } else if (psk_offered_) {
        // Rejected: the PSK-colored early tree is wrong — a fresh no-PSK
        // schedule over the negotiated suite.
        sched_.reset();
    }
    if (!sched_.has_value()) {
        // The no-PSK (or post-HRR) flow owns no schedule until the suite is
        // known — the SH read point is where it starts.
        sched_.emplace(suite_);
    }
    early_.closed = true;

    const TlsKxShared z = hello_.kx->decap(sh.key_share);
    if (z.status == TlsKxStatus::BadPeerData) {
        fail(TlsAlertDesc::IllegalParameter);
        return;
    }
    if (z.status != TlsKxStatus::Ok) {
        fail(TlsAlertDesc::InternalError);
        return;
    }

    // Transcript from the SH read point (06 §5.2): the retained CH bytes
    // first, then the SH. After an HRR the transcript already holds
    // message_hash + CH2 — only the SH is appended.
    if (hrr_count_ == 0) {
        if (!t13_.init(info->hash) || !t13_.update({hello_.ch.data(), hello_.len})) {
            fail(TlsAlertDesc::InternalError);
            return;
        }
    }
    feed13(TlsHandshakeType::ServerHello, body);
    snapshot13(); // Hash(CH..SH)

    if (!sched_->handshake_secrets({z.z.data(), z.z.size()}, {hash_buf_.data(), hash_len()}, client_hs_, server_hs_)
                 .has_value()) {
        fail(TlsAlertDesc::InternalError);
        return;
    }
    if (!swap_cipher(ctx_.read_cipher(), server_hs_)) {
        fail(TlsAlertDesc::InternalError);
        return;
    }
    ctx_.set_inbound_mode(TlsInboundMode::Sealed13);
    st_ = St::ExpectEe;
}

void Tls13ClientHandshake::start_hello_retry_request(const TlsServerHello &sh,
                                                     std::span<const std::uint8_t> body) noexcept {
    if (hrr_count_ >= 1) {
        fail(TlsAlertDesc::UnexpectedMessage); // a second HRR is a MUST-abort
        return;
    }
    if (sh.has_alpn || sh.has_extended_master_secret || sh.has_renegotiation_info) {
        // Extensions that cannot appear in an SH-family message.
        fail(TlsAlertDesc::UnsupportedExtension);
        return;
    }
    if (sh.compression_method != 0 || !sh.has_key_share || !sh.key_share.empty() || sh.has_selected_identity) {
        // Wrong-shaped HRR: missing selected_group key_share, a server share,
        // or a PSK selection that belongs in the real SH.
        fail(TlsAlertDesc::IllegalParameter);
        return;
    }
    const TlsSuiteInfo *info = tls_suite_info(static_cast<TlsCipherSuiteId>(sh.cipher_suite));
    if (tls_client_suite_offer_index(sh.cipher_suite) == kOfferedSuites.size() || info == nullptr || !info->is_tls13) {
        fail(TlsAlertDesc::IllegalParameter);
        return;
    }
    if (sh.session_id.size() != hello_.session_id.size() ||
        std::memcmp(sh.session_id.data(), hello_.session_id.data(), hello_.session_id.size()) != 0) {
        fail(TlsAlertDesc::IllegalParameter);
        return;
    }
    const std::uint16_t selected = sh.key_share_group;
    if (!tls_client_group_offered(selected) || selected == static_cast<std::uint16_t>(hello_.kx_group)) {
        // A group we never offered — or the one we already sent a share for
        // (the server should have used that share) — is a protocol fault.
        fail(TlsAlertDesc::IllegalParameter);
        return;
    }

    // Transcript restart (RFC 8446 §4.4.1): the synthetic message_hash wraps
    // ONLY Hash(CH1); the HRR itself is hashed as the first message of the
    // restarted transcript (BoringSSL hashes CH1, calls
    // UpdateForHelloRetryRequest, and add_message(HRR) lands after the
    // restart) — pinned by interop: Hash(msg_hash(Hash(CH1)) || HRR || CH2 || SH).
    if (!t13_.init(info->hash) || !t13_.update({hello_.ch.data(), hello_.len}) || !t13_.restart_message_hash()) {
        fail(TlsAlertDesc::InternalError);
        return;
    }
    feed13(TlsHandshakeType::ServerHello, body); // HRR is hashed as an SH

    // The 02 contract: schedule and key exchange are destroyed, not reused
    // (the restart invalidates their state); 0-RTT died with the HRR.
    hello_.kx.reset();
    sched_.reset();
    early_.write = TlsRecordCipher{};
    early_.closed = true;
    early_.offered_ext = false;

    if (!tls_client_hello_build(hello_, cfg_, session_, psk_offered_, early_.offered_ext, true, selected,
                                sh.has_cookie ? sh.cookie : std::span<const std::uint8_t>{})) {
        fail(TlsAlertDesc::InternalError);
        return;
    }
    if (psk_offered_) {
        // PSK survives the HRR; the binder is recomputed over truncated CH2
        // with a fresh schedule (binder_key is once-per-instance).
        sched_.emplace(session_->suite);
        if (!sched_->set_psk(session_->psk).has_value() || !tls_client_backfill_psk_binder(*sched_, hello_)) {
            fail(TlsAlertDesc::InternalError);
            return;
        }
    }
    // The one compat CCS flushes immediately before the second flight — here
    // CH2 — at legacy_record_version 0x0303 (BoringSSL queues it there; after
    // CH1 it only went out with early data, hence the once-flag).
    if ((!ccs_sent_ && !ctx_.send_ccs().has_value()) || !t13_.update({hello_.ch.data(), hello_.len}) ||
        !emit_message({hello_.ch.data(), hello_.len})) {
        fail(TlsAlertDesc::InternalError);
        return;
    }
    ccs_sent_ = true;
    hrr_count_ = 1;
    hrr_suite_ = static_cast<TlsCipherSuiteId>(sh.cipher_suite);
    // st_ stays WaitServerHello — the real SH is the next message.
}

// =====================================================================
// Post-fork inbound traffic
// =====================================================================

void Tls13ClientHandshake::on_message(TlsHandshakeType type, std::span<const std::uint8_t> body) noexcept {
    switch (st_) {
        case St::WaitServerHello:
            if (type != TlsHandshakeType::ServerHello) {
                fail(TlsAlertDesc::UnexpectedMessage);
                return;
            }
            handle_server_hello(body);
            return;
        case St::ExpectEe:
            if (type != TlsHandshakeType::EncryptedExtensions) {
                fail(TlsAlertDesc::UnexpectedMessage);
                return;
            }
            handle_encrypted_extensions(body);
            return;
        case St::ExpectCrCertFin:
            switch (type) {
                case TlsHandshakeType::CertificateRequest:
                    handle_certificate_request_13(body);
                    return;
                case TlsHandshakeType::Certificate:
                    handle_certificate_13(body);
                    return;
                case TlsHandshakeType::Finished:
                    handle_finished_13(body);
                    return;
                default:
                    fail(TlsAlertDesc::UnexpectedMessage);
                    return;
            }
        case St::ExpectCv:
            if (type != TlsHandshakeType::CertificateVerify) {
                fail(TlsAlertDesc::UnexpectedMessage);
                return;
            }
            handle_certificate_verify_13(body);
            return;
        case St::ExpectFin:
            if (type != TlsHandshakeType::Finished) {
                fail(TlsAlertDesc::UnexpectedMessage);
                return;
            }
            handle_finished_13(body);
            return;
        case St::Done:
            FIBER_ASSERT(false); // the pump stops at done; a message here is an engine bug
            return;
    }
}

void Tls13ClientHandshake::handle_server_hello(std::span<const std::uint8_t> body) noexcept {
    // The real SH after an HRR (WaitServerHello only recurs post-HRR). The
    // outer shell fixed the version at its fork decision; this re-run checks
    // the same invariants and dispatches — a second HRR aborts in the HRR
    // handler (hrr_count_).
    TlsServerHello sh;
    if (!tls_decode_server_hello(body.data(), body.size(), sh).has_value()) {
        fail(TlsAlertDesc::DecodeError);
        return;
    }
    if (!sh.has_supported_version || sh.supported_version != kTlsVersionTls13) {
        // An HRR committed the server to 1.3 (RFC 8446 §4.1.4); a 1.2-shaped
        // SH afterwards is a protocol fault, not a 1.2 handshake.
        fail(TlsAlertDesc::IllegalParameter);
        return;
    }
    if (tls_is_hello_retry_request(sh.random)) {
        start_hello_retry_request(sh, body);
        return;
    }
    start(sh, body);
}

// =====================================================================
// 1.3 server flight
// =====================================================================

void Tls13ClientHandshake::handle_encrypted_extensions(std::span<const std::uint8_t> body) noexcept {
    TlsEncryptedExtensions ee;
    if (!tls_decode_encrypted_extensions(body.data(), body.size(), ee).has_value()) {
        fail(TlsAlertDesc::DecodeError);
        return;
    }
    // Extensions that only exist in the CH/SH/HRR must not reappear in EE
    // (RFC 8446 §4.2: the client MUST abort with unsupported_extension).
    TlsExtensionView view;
    for (TlsExtensionCursor walk(ee.extensions_block);;) {
        const auto entry = walk.next(view);
        if (!entry.has_value()) {
            fail(TlsAlertDesc::DecodeError);
            return;
        }
        if (!entry.value()) {
            break;
        }
        switch (static_cast<TlsExtensionType>(view.type)) {
            case TlsExtensionType::SupportedVersions:
            case TlsExtensionType::KeyShare:
            case TlsExtensionType::Cookie:
            case TlsExtensionType::PreSharedKey:
                fail(TlsAlertDesc::UnsupportedExtension);
                return;
            default:
                break;
        }
    }

    if (ee.has_early_data && (!early_.offered_ext || !psk_accepted_)) {
        // The server accepted early data we never validly offered (a rejected
        // PSK kills any early_data offer along with it).
        fail(TlsAlertDesc::IllegalParameter);
        return;
    }
    feed13(TlsHandshakeType::EncryptedExtensions, body);

    if (ee.has_alpn) {
        bool offered = false;
        for (const std::string_view name: cfg_.alpn) {
            if (name == ee.alpn) {
                offered = true;
                break;
            }
        }
        if (!offered || ee.alpn.size() > state_.alpn.size()) {
            fail(TlsAlertDesc::IllegalParameter);
            return;
        }
        std::memcpy(state_.alpn.data(), ee.alpn.data(), ee.alpn.size());
        state_.alpn_len = static_cast<std::uint16_t>(ee.alpn.size());
    }
    ee_early_data_ = ee.has_early_data;
    st_ = St::ExpectCrCertFin;
}

void Tls13ClientHandshake::handle_certificate_request_13(std::span<const std::uint8_t> body) noexcept {
    if (cr13_received_) {
        fail(TlsAlertDesc::UnexpectedMessage); // at most one CR, right after EE
        return;
    }
    TlsCertificateRequest13 cr;
    if (!tls_decode_certificate_request_13(body.data(), body.size(), cr).has_value()) {
        fail(TlsAlertDesc::DecodeError);
        return;
    }
    if (cr.signature_algorithms.size() % 2 != 0 || cr.signature_algorithms.size() / 2 > cr_sigalgs_.size()) {
        fail(TlsAlertDesc::DecodeError);
        return;
    }
    feed13(TlsHandshakeType::CertificateRequest, body);
    cr13_received_ = true;
    cr_ctx_len_ = cr.certificate_request_context.size();
    if (cr_ctx_len_ > 0) {
        std::memcpy(cr_context_.data(), cr.certificate_request_context.data(), cr_ctx_len_);
    }
    cr_sigalgs_n_ = cr.signature_algorithms.size() / 2;
    for (std::size_t i = 0; i < cr_sigalgs_n_; ++i) {
        cr_sigalgs_[i] =
                static_cast<std::uint16_t>((cr.signature_algorithms[2 * i] << 8) | cr.signature_algorithms[2 * i + 1]);
    }
}

void Tls13ClientHandshake::handle_certificate_13(std::span<const std::uint8_t> body) noexcept {
    if (psk_accepted_) {
        fail(TlsAlertDesc::UnexpectedMessage); // resumed sessions carry no Cert/CV
        return;
    }
    TlsCertificate13 cert;
    if (!tls_decode_certificate_13(body.data(), body.size(), cert).has_value() || cert.cert_count == 0) {
        fail(TlsAlertDesc::DecodeError); // a server chain is never empty
        return;
    }
    auto chain = TlsCertificateChain::from_der_list({cert.certs, cert.cert_count});
    if (!chain.has_value()) {
        fail(TlsAlertDesc::DecodeError);
        return;
    }
    peer_chain_ = std::move(chain).value();
    feed13(TlsHandshakeType::Certificate, body);

    if (cfg_.verify_peer) {
        const auto verification = tls_verify_chain(peer_chain_, *cfg_.trust, TlsCertPurpose::SslServer, cfg_.sni_host,
                                                   cfg_.verify_ip, cfg_.now_unix_ms);
        if (!verification.has_value()) {
            fail(TlsAlertDesc::InternalError);
            return;
        }
        if (verification->status == TlsCertVerification::Status::NotTrusted) {
            fail(verification->alert);
            return;
        }
    }
    st_ = St::ExpectCv;
}

void Tls13ClientHandshake::handle_certificate_verify_13(std::span<const std::uint8_t> body) noexcept {
    TlsCertificateVerify cv;
    if (!tls_decode_certificate_verify(body.data(), body.size(), cv).has_value()) {
        fail(TlsAlertDesc::DecodeError);
        return;
    }
    // The signature covers the transcript through Certificate — snapshotted
    // BEFORE this message itself is fed (06 §5.2 ordering).
    snapshot13();

    bool offered = false;
    for (const TlsSignatureScheme scheme: kTls13SignaturePreference) {
        if (static_cast<std::uint16_t>(scheme) == cv.algorithm) { // 1.3-valid ⊆ our CH offer
            offered = true;
            break;
        }
    }
    if (!offered) {
        fail(TlsAlertDesc::IllegalParameter);
        return;
    }
    const auto public_key = peer_chain_.leaf().public_key();
    if (!public_key.has_value()) {
        fail(TlsAlertDesc::InternalError);
        return;
    }
    std::array<std::uint8_t, kCvContentMax> content{};
    const std::size_t content_len = 64 + kServerCvCtxLen + 1 + hash_len();
    build_cert_verify_content({content.data(), content_len}, {"TLS 1.3, server CertificateVerify", kServerCvCtxLen},
                              {hash_buf_.data(), hash_len()});
    const auto valid = tls_verify(static_cast<TlsSignatureScheme>(cv.algorithm), TlsProtocolVersion::Tls13, *public_key,
                                  {content.data(), content_len}, cv.signature);
    if (!valid.has_value()) {
        fail(TlsAlertDesc::IllegalParameter); // scheme does not match the peer's key
        return;
    }
    if (!valid.value()) {
        fail(TlsAlertDesc::DecryptError);
        return;
    }
    feed13(TlsHandshakeType::CertificateVerify, body);
    st_ = St::ExpectFin;
}

void Tls13ClientHandshake::handle_finished_13(std::span<const std::uint8_t> body) noexcept {
    if (st_ == St::ExpectCrCertFin && !psk_accepted_) {
        fail(TlsAlertDesc::UnexpectedMessage); // Fin without a certificate flight
        return;
    }
    TlsFinished fin;
    if (!tls_decode_finished(body.data(), body.size(), fin).has_value() || fin.verify_data.size() != hash_len()) {
        fail(TlsAlertDesc::DecodeError);
        return;
    }
    // verify_data covers the transcript through whatever preceded Fin —
    // snapshotted before Fin itself is fed.
    snapshot13();
    std::array<std::uint8_t, 48> expected{};
    if (!tls13_finished_mac(server_hs_, {hash_buf_.data(), hash_len()}, {expected.data(), hash_len()}).has_value()) {
        fail(TlsAlertDesc::InternalError);
        return;
    }
    if (!tls_constant_time_equal({expected.data(), hash_len()}, fin.verify_data)) {
        fail(TlsAlertDesc::DecryptError);
        return;
    }
    feed13(TlsHandshakeType::Finished, body);
    finish_1_3();
}

// Client Cert (+ CV when a credential is configured; an empty Cert when not).
// False = failure the caller raises (handshake_failure / internal_error).
bool Tls13ClientHandshake::send_client_certificate_13() noexcept {
    const bool have_credential =
            cfg_.client_chain != nullptr && cfg_.client_key != nullptr && !cfg_.client_chain->empty();
    if (!have_credential) {
        // No credential: an empty chain — structurally valid, the server
        // decides whether that ends the handshake.
        const auto cert_len =
                tls_encode_certificate_13({cr_context_.data(), cr_ctx_len_}, {}, {scratch_.data(), scratch_.size()});
        if (!cert_len.has_value() || !t13_.update({scratch_.data(), cert_len.value()}) ||
            !emit_message({scratch_.data(), cert_len.value()})) {
            return false;
        }
        return true;
    }

    const TlsCertificateChain &chain = *cfg_.client_chain;
    std::array<std::span<const std::uint8_t>, TlsCertificateChain::kMaxCerts> ders{};
    ders[0] = chain.leaf().der();
    for (std::size_t i = 0; i < chain.intermediates().size(); ++i) {
        ders[i + 1] = chain.intermediates()[i].der();
    }
    const auto cert_len = tls_encode_certificate_13({cr_context_.data(), cr_ctx_len_}, {ders.data(), chain.size()},
                                                    {scratch_.data(), scratch_.size()});
    if (!cert_len.has_value() || !t13_.update({scratch_.data(), cert_len.value()}) ||
        !emit_message({scratch_.data(), cert_len.value()})) {
        return false;
    }

    // CertificateVerify: first 1.3 preference the server offered in its CR
    // that the local key supports (02b selection loop).
    snapshot13(); // through our Cert
    std::array<std::uint8_t, kCvContentMax> content{};
    const std::size_t content_len = 64 + kClientCvCtxLen + 1 + hash_len();
    build_cert_verify_content({content.data(), content_len}, {"TLS 1.3, client CertificateVerify", kClientCvCtxLen},
                              {hash_buf_.data(), hash_len()});
    std::optional<TlsSignatureScheme> chosen;
    for (const TlsSignatureScheme scheme: kTls13SignaturePreference) {
        if (!cfg_.client_key->supports(scheme, TlsProtocolVersion::Tls13)) {
            continue;
        }
        for (std::size_t i = 0; i < cr_sigalgs_n_; ++i) {
            if (cr_sigalgs_[i] == static_cast<std::uint16_t>(scheme)) {
                chosen = scheme;
                break;
            }
        }
        if (chosen.has_value()) {
            break;
        }
    }
    if (!chosen.has_value()) {
        return false; // no common scheme — caller raises handshake_failure
    }
    std::array<std::uint8_t, kClientMaxSigLen> sig{};
    FIBER_ASSERT(cfg_.client_key->max_signature_len() <= sig.size());
    const auto sig_len = cfg_.client_key->sign(*chosen, TlsProtocolVersion::Tls13, {content.data(), content_len}, sig);
    if (!sig_len.has_value()) {
        return false;
    }
    const auto cv_len = tls_encode_certificate_verify(
            static_cast<std::uint16_t>(*chosen), {sig.data(), sig_len.value()}, {scratch_.data(), scratch_.size()});
    if (!cv_len.has_value() || !t13_.update({scratch_.data(), cv_len.value()}) ||
        !emit_message({scratch_.data(), cv_len.value()})) {
        return false;
    }
    return true;
}

void Tls13ClientHandshake::finish_1_3() noexcept {
    // Application secrets over Hash(CH..server Fin) — the transcript already
    // includes the verified Fin. The read side swaps to a fresh server_app0
    // instance now; the write side rides client_hs for the flight below and
    // swaps last (06 §5.3).
    snapshot13();
    if (!sched_->application_secrets({hash_buf_.data(), hash_len()}, client_app0_, server_app0_).has_value() ||
        !swap_cipher(ctx_.read_cipher(), server_app0_) || !swap_cipher(ctx_.write_cipher(), client_hs_)) {
        fail(TlsAlertDesc::InternalError);
        return;
    }

    // ---- client flight (sealed with the client_hs cipher) ----
    // Plain-path compat CCS: immediately before the second flight (RFC 8446
    // D.4), plaintext 0x0303 — the peer's record layer discards it before
    // routing; skipped when the early-data or HRR path already sent it.
    if (!ccs_sent_ && !ctx_.send_ccs().has_value()) {
        fail(TlsAlertDesc::InternalError);
        return;
    }
    ccs_sent_ = true;
    if (cr13_received_ && !send_client_certificate_13()) {
        fail(TlsAlertDesc::HandshakeFailure);
        return;
    }
    if (early_.offered_ext && psk_accepted_ && ee_early_data_) {
        // EndOfEarlyData closes the accepted 0-RTT window.
        const auto eoed =
                tls_encode_handshake_message(TlsHandshakeType::EndOfEarlyData, {}, {scratch_.data(), scratch_.size()});
        if (!eoed.has_value() || !t13_.update({scratch_.data(), eoed.value()}) ||
            !emit_message({scratch_.data(), eoed.value()})) {
            fail(TlsAlertDesc::InternalError);
            return;
        }
    }
    early_.closed = true;

    // client Finished — MAC over the transcript before itself.
    snapshot13();
    std::array<std::uint8_t, 48> verify_data{};
    if (!tls13_finished_mac(client_hs_, {hash_buf_.data(), hash_len()}, {verify_data.data(), hash_len()}).has_value()) {
        fail(TlsAlertDesc::InternalError);
        return;
    }
    const auto fin_len = tls_encode_finished({verify_data.data(), hash_len()}, {scratch_.data(), scratch_.size()});
    if (!fin_len.has_value() || !t13_.update({scratch_.data(), fin_len.value()}) ||
        !emit_message({scratch_.data(), fin_len.value()})) {
        fail(TlsAlertDesc::InternalError);
        return;
    }

    // Resumption base over Hash(CH..client Fin) — after feeding it — then the
    // write side swaps to client_app0; the hs instance dies with the flight.
    snapshot13();
    auto resumption = sched_->resumption_master_secret({hash_buf_.data(), hash_len()});
    if (!resumption.has_value() || !swap_cipher(ctx_.write_cipher(), client_app0_)) {
        fail(TlsAlertDesc::InternalError);
        return;
    }
    resumption_master_ = std::move(resumption).value();

    // ---- Done ----
    state_.version = TlsProtocolVersion::Tls13;
    state_.suite = suite_;
    state_.read_cipher = std::move(ctx_.read_cipher());
    state_.write_cipher = std::move(ctx_.write_cipher());
    state_.client_app_secret = std::move(client_app0_);
    state_.server_app_secret = std::move(server_app0_);
    state_.resumption_master = std::move(resumption_master_);
    state_.session_resumed = psk_accepted_;
    state_.early_data_accepted = early_.offered_ext && psk_accepted_ && ee_early_data_;
    state_.peer_chain = std::move(peer_chain_);
    out_.done = true;
    st_ = St::Done;
    // Reader bytes arriving after the server Fin (e.g. an NST in the same
    // feed) stay buffered — post-handshake messages belong to 08/09.
}

} // namespace fiber::tls
