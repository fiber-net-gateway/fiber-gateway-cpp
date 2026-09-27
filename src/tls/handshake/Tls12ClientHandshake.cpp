#include "Tls12ClientHandshake.h"

#include <cstring>
#include <utility>

#include "../crypto/TlsCryptoPrimitives.h"

#include <fiber/tls/TlsVersion.h>
#include <fiber/tls/crypto/Tls12KeySchedule.h>
#include <fiber/tls/crypto/TlsCertificate.h>
#include <fiber/tls/crypto/TlsSignature.h>
#include <fiber/tls/record/TlsRecord.h>

namespace fiber::tls {

Tls12ClientHandshake::Tls12ClientHandshake(const Mount &mount) noexcept :
    ctx_(mount.ctx), cfg_(mount.cfg), hello_(mount.hello), out_(mount.out), scratch_(mount.scratch) {}

Tls12ClientHandshake::~Tls12ClientHandshake() {
    // Explicit wipes at the handoff points the 02 contract names; the key
    // exchange wipes itself. master12_ already moved into state_ arrives here
    // moved-from (pre-wiped).
    master12_.wipe();
    wipe_kb12();
    tls_secure_wipe(z12_.z.data(), z12_.z.size());
}

// =====================================================================
// Entry: ServerHello (1.2)
// =====================================================================

void Tls12ClientHandshake::start(const TlsServerHello &sh, std::span<const std::uint8_t> body) noexcept {
    // The CH offered [0x0304, 0x0303]; anything but 0x0303 here is a version
    // we never offered (and any 0-RTT window died with the 1.2 negotiation —
    // the outer shell closed it at the fork).
    if (sh.legacy_version != kTlsVersionTls12) {
        fail(TlsAlertDesc::ProtocolVersion);
        return;
    }
    // RFC 8446 §4.1.3 downgrade sentinel: a 1.3-capable server forced down to
    // 1.2 flags itself with DOWNGRD||0x01 in the last 8 random bytes — the
    // downgrade was injected, the handshake is dead. Only a 1.3-capable
    // client checks: one pinned to 1.2 never offered 1.3, so its CH "not
    // supporting 1.3" is genuine, not a strip (and such servers send the
    // sentinel per §4.1.3 — harmless to us).
    static constexpr std::array<std::uint8_t, 8> kDowngradeSentinel{0x44, 0x4F, 0x57, 0x4E, 0x47, 0x52, 0x44, 0x01};
    if (cfg_.max_version >= kTlsVersionTls13 &&
        std::memcmp(sh.random.data() + 24, kDowngradeSentinel.data(), kDowngradeSentinel.size()) == 0) {
        fail(TlsAlertDesc::IllegalParameter);
        return;
    }
    if (sh.compression_method != 0) {
        fail(TlsAlertDesc::IllegalParameter);
        return;
    }
    if (sh.has_key_share || sh.has_cookie) {
        fail(TlsAlertDesc::UnsupportedExtension); // 1.3-only extensions
        return;
    }
    if (sh.has_selected_identity) {
        fail(TlsAlertDesc::IllegalParameter); // PSK selection is 1.3-only
        return;
    }
    // RFC 5746 §3.4: we offered the empty initial RI; the echo must be present
    // and empty — the payload is exactly the 1-byte length prefix 0x00 of an
    // empty renegotiated_connection. Missing or renegotiation-bearing
    // (longer) both end the handshake.
    if (!sh.has_renegotiation_info || sh.renegotiation_info.size() != 1 || sh.renegotiation_info[0] != 0) {
        fail(TlsAlertDesc::HandshakeFailure);
        return;
    }
    if (!tls_client_suite_offered(sh.cipher_suite)) {
        fail(TlsAlertDesc::IllegalParameter); // never offered
        return;
    }
    suite_ = static_cast<TlsCipherSuiteId>(sh.cipher_suite);
    const TlsSuiteInfo *info = suite_info();
    if (info == nullptr || info->is_tls13) {
        fail(TlsAlertDesc::IllegalParameter); // a 1.3 suite in a 1.2 SH
        return;
    }
    if (sh.has_alpn) {
        bool offered = false;
        for (const std::string_view name: cfg_.alpn) {
            if (name == sh.alpn) {
                offered = true;
                break;
            }
        }
        if (!offered || sh.alpn.size() > state_.alpn.size()) {
            fail(TlsAlertDesc::IllegalParameter); // RFC 7301 §3.1
            return;
        }
        std::memcpy(state_.alpn.data(), sh.alpn.data(), sh.alpn.size());
        state_.alpn_len = static_cast<std::uint16_t>(sh.alpn.size());
    }

    std::memcpy(server_random_.data(), sh.random.data(), server_random_.size());
    ems_negotiated_ = sh.has_extended_master_secret; // we always offer EMS
    // Server records stay plaintext until its CCS; the session_id echo is the
    // server's own choice in 1.2 (it names ITS session, not ours).
    ctx_.set_inbound_mode(TlsInboundMode::Plaintext12);

    if (!t12_.init(info->hash) || !t12_.update({hello_.ch.data(), hello_.len})) {
        fail(TlsAlertDesc::InternalError);
        return;
    }
    feed12(TlsHandshakeType::ServerHello, body);
    st_ = St::ExpectServerCert12;
}

// =====================================================================
// Post-fork inbound traffic
// =====================================================================

void Tls12ClientHandshake::on_message(TlsHandshakeType type, std::span<const std::uint8_t> body) noexcept {
    switch (st_) {
        case St::ExpectServerCert12:
            if (type != TlsHandshakeType::Certificate) {
                fail(TlsAlertDesc::UnexpectedMessage);
                return;
            }
            handle_certificate_12(body);
            return;
        case St::ExpectSke12:
            if (type != TlsHandshakeType::ServerKeyExchange) {
                fail(TlsAlertDesc::UnexpectedMessage);
                return;
            }
            handle_server_key_exchange_12(body);
            return;
        case St::ExpectCrShd12:
            switch (type) {
                case TlsHandshakeType::CertificateRequest:
                    handle_certificate_request_12(body);
                    return;
                case TlsHandshakeType::ServerHelloDone:
                    handle_server_hello_done_12(body);
                    return;
                default:
                    fail(TlsAlertDesc::UnexpectedMessage);
                    return;
            }
        case St::ExpectServerCcs12:
            // Handshake traffic between our flight and the server's CCS is
            // limited to session tickets; everything else waits for the cipher.
            if (type != TlsHandshakeType::NewSessionTicket) {
                fail(TlsAlertDesc::UnexpectedMessage);
                return;
            }
            handle_new_session_ticket_12(body);
            return;
        case St::ExpectServerFin12:
            if (type != TlsHandshakeType::Finished) {
                fail(TlsAlertDesc::UnexpectedMessage);
                return;
            }
            handle_finished_12(body);
            return;
        case St::Done:
            FIBER_ASSERT(false); // the pump stops at done; a message here is an engine bug
            return;
    }
}

void Tls12ClientHandshake::on_ccs() noexcept {
    switch (st_) {
        case St::ExpectServerCcs12:
            // The server's one CCS switches the read side to the key_block
            // server keys; our write side went live with the client flight.
            if (!swap_cipher_12(ctx_.read_cipher(), TlsRecordDirection::Open, kb12_.server)) {
                fail(TlsAlertDesc::InternalError);
                return;
            }
            ctx_.set_inbound_mode(TlsInboundMode::Sealed12);
            st_ = St::ExpectServerFin12;
            break;
        case St::ExpectServerCert12:
        case St::ExpectSke12:
        case St::ExpectCrShd12:
            // The server's CCS belongs after its flight and its tickets,
            // right before its Finished — earlier than that is a fault.
            fail(TlsAlertDesc::UnexpectedMessage);
            break;
        default:
            // A repeat CCS at/after the expected point is middlebox noise;
            // the context validated the 1-byte 0x01 form.
            break;
    }
}

// =====================================================================
// 1.2 server flight
// =====================================================================

void Tls12ClientHandshake::handle_certificate_12(std::span<const std::uint8_t> body) noexcept {
    TlsCertificate12 cert;
    if (!tls_decode_certificate_12(body.data(), body.size(), cert).has_value() || cert.cert_count == 0) {
        fail(TlsAlertDesc::DecodeError); // a server chain is never empty
        return;
    }
    auto chain = TlsCertificateChain::from_der_list({cert.certs, cert.cert_count});
    if (!chain.has_value()) {
        fail(TlsAlertDesc::DecodeError);
        return;
    }
    peer_chain_ = std::move(chain).value();
    feed12(TlsHandshakeType::Certificate, body);

    // The leaf must fit the suite's auth half (BoringSSL
    // ssl_check_leaf_certificate): RSA suites need an RSA key, ECDSA suites
    // an EC or Ed25519 one (RFC 8422 §5.1). The SKE check alone cannot catch
    // an ECDHE-RSA suite over an EC leaf whose SKE is ECDSA-signed.
    const auto leaf_key = peer_chain_.leaf().public_key();
    if (!leaf_key.has_value()) {
        fail(TlsAlertDesc::InternalError);
        return;
    }
    if ((leaf_key->key_kind() == TlsKeyKind::Rsa) != (suite_info()->auth == TlsSuiteAuth::Rsa)) {
        fail(TlsAlertDesc::IllegalParameter);
        return;
    }

    if (cfg_.verify_peer) {
        // The check name is check_host when set, else the SNI send name
        // (09 §4.3: the net layer's server_name/verify_name split).
        const std::string_view check_host = cfg_.check_host.empty() ? cfg_.sni_host : cfg_.check_host;
        const auto verification = tls_verify_chain(peer_chain_, *cfg_.trust, TlsCertPurpose::SslServer, check_host,
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
    // Static RSA (feature/tls/11) sends no ServerKeyExchange: the premaster
    // is encrypted to this leaf in our flight. A stray SKE then falls to the
    // CR/SHD state's unexpected_message.
    st_ = suite_info()->kx == TlsSuiteKx::Rsa ? St::ExpectCrShd12 : St::ExpectSke12;
}

void Tls12ClientHandshake::handle_server_key_exchange_12(std::span<const std::uint8_t> body) noexcept {
    TlsServerKeyExchange ske;
    if (!tls_decode_server_key_exchange(body.data(), body.size(), ske).has_value()) {
        fail(TlsAlertDesc::IllegalParameter); // e.g. non-named_curve forms
        return;
    }
    if (!tls_client_group_offered(ske.named_group) || !tls_client_sigalg_offered(ske.algorithm)) {
        fail(TlsAlertDesc::IllegalParameter);
        return;
    }

    // The signature covers the 1.2 "ServerDHParams" prefixed with both
    // randoms — client_random || server_random || curve_type(3) ||
    // be16(group) || u8(point_len) || point (BoringSSL handshake_server.cc
    // builds and signs exactly these bytes; it is NOT a transcript digest).
    static constexpr std::size_t kSkeContentMax = 64 + 1 + 2 + 1 + 255; // randoms + params + u8 point cap
    std::array<std::uint8_t, kSkeContentMax> content{};
    std::memcpy(content.data(), hello_.client_random.data(), hello_.client_random.size());
    std::memcpy(content.data() + 32, server_random_.data(), server_random_.size());
    std::size_t off = 64;
    content[off++] = 0x03; // curve_type named_curve
    content[off++] = static_cast<std::uint8_t>(ske.named_group >> 8);
    content[off++] = static_cast<std::uint8_t>(ske.named_group);
    content[off++] = static_cast<std::uint8_t>(ske.public_key.size());
    std::memcpy(content.data() + off, ske.public_key.data(), ske.public_key.size());
    off += ske.public_key.size();

    const auto public_key = peer_chain_.leaf().public_key();
    if (!public_key.has_value()) {
        fail(TlsAlertDesc::InternalError);
        return;
    }
    const auto valid = tls_verify(static_cast<TlsSignatureScheme>(ske.algorithm), TlsProtocolVersion::Tls12,
                                  *public_key, {content.data(), off}, ske.signature);
    if (!valid.has_value()) {
        fail(TlsAlertDesc::IllegalParameter); // scheme does not match the peer's key
        return;
    }
    if (!valid.value()) {
        fail(TlsAlertDesc::DecryptError);
        return;
    }

    // The server's curve wins: rebuild the ephemeral pair when it differs
    // from the group of the CH share (the 1.2 server ignores key_share — the
    // curve arrives here, in the SKE).
    const auto group = static_cast<TlsNamedGroup>(ske.named_group);
    if (group != hello_.kx_group) {
        auto kx = tls_client_kx_offer(group);
        if (!kx.has_value()) {
            fail(TlsAlertDesc::InternalError);
            return;
        }
        hello_.kx = std::move(*kx);
        hello_.kx_group = group;
    }
    const TlsKxShared z = hello_.kx->decap(ske.public_key);
    if (z.status == TlsKxStatus::BadPeerData) {
        fail(TlsAlertDesc::IllegalParameter);
        return;
    }
    if (z.status != TlsKxStatus::Ok) {
        fail(TlsAlertDesc::InternalError);
        return;
    }
    z12_ = z;

    feed12(TlsHandshakeType::ServerKeyExchange, body);
    st_ = St::ExpectCrShd12;
}

void Tls12ClientHandshake::handle_certificate_request_12(std::span<const std::uint8_t> body) noexcept {
    if (cr12_received_) {
        fail(TlsAlertDesc::UnexpectedMessage); // at most one CR per flight
        return;
    }
    TlsCertificateRequest12 cr;
    if (!tls_decode_certificate_request_12(body.data(), body.size(), cr).has_value()) {
        fail(TlsAlertDesc::DecodeError);
        return;
    }
    if (cr.signature_algorithms.size() % 2 != 0) {
        fail(TlsAlertDesc::DecodeError);
        return;
    }
    feed12(TlsHandshakeType::CertificateRequest, body);
    cr12_received_ = true;
    cr_sigalgs_present_ = cr.has_signature_algorithms; // absent = no constraint
    cr_sigalgs_n_ = tls_client_keep_cr_sigalgs(cr.signature_algorithms, kTls12SignaturePreference, cr_sigalgs_);
    // stays in ExpectCrShd12 — ServerHelloDone still terminates the flight
}

void Tls12ClientHandshake::handle_server_hello_done_12(std::span<const std::uint8_t> body) noexcept {
    if (!body.empty()) {
        fail(TlsAlertDesc::DecodeError); // SHD carries no payload
        return;
    }
    feed12(TlsHandshakeType::ServerHelloDone, body);
    if (!send_client_flight_12()) {
        return; // the flight raised its own alert
    }
    st_ = St::ExpectServerCcs12;
}

// [Cert [CV]] CKE CCS Fin — every failure path raises its own alert. The
// client Cert/CKE/CV fly plaintext; the CCS swaps the write cipher, so the
// client Fin is the first sealed record.
bool Tls12ClientHandshake::send_client_flight_12() noexcept {
    // ---- client Certificate (only under a CertificateRequest) ----
    if (cr12_received_) {
        const bool have_credential =
                cfg_.client_chain != nullptr && cfg_.client_key != nullptr && !cfg_.client_chain->empty();
        std::size_t cert_len = 0;
        if (have_credential) {
            // 1.2 chain form: leaf + intermediates, no request context.
            const TlsCertificateChain &chain = *cfg_.client_chain;
            std::array<std::span<const std::uint8_t>, TlsCertificateChain::kMaxCerts> ders{};
            ders[0] = chain.leaf().der();
            for (std::size_t i = 0; i < chain.intermediates().size(); ++i) {
                ders[i + 1] = chain.intermediates()[i].der();
            }
            const auto encoded =
                    tls_encode_certificate_12({ders.data(), chain.size()}, {scratch_.data(), scratch_.size()});
            if (!encoded.has_value()) {
                fail(TlsAlertDesc::InternalError);
                return false;
            }
            cert_len = encoded.value();
            sent_cert12_ = true;
        } else {
            // No credential: an empty chain — structurally valid; the server
            // that asked decides whether that ends the handshake.
            const auto encoded = tls_encode_certificate_12({}, {scratch_.data(), scratch_.size()});
            if (!encoded.has_value()) {
                fail(TlsAlertDesc::InternalError);
                return false;
            }
            cert_len = encoded.value();
        }
        if (!t12_.update({scratch_.data(), cert_len}) || !emit_message({scratch_.data(), cert_len})) {
            fail(TlsAlertDesc::InternalError);
            return false;
        }
    }

    // ---- ClientKeyExchange (plaintext) ----
    // ECDHE: u8(point_len) || point. Static RSA (RFC 5246 §7.4.7.1): the
    // premaster is our ClientHello's client_version (0x0303) || 46 random
    // bytes, RSAES-PKCS1-v1_5 encrypted to the leaf key (its RSA kind was
    // checked at the Certificate), u16-length-prefixed.
    common::IoResult<std::size_t> cke_len = std::unexpected(common::IoErr::Invalid);
    if (suite_info()->kx == TlsSuiteKx::Rsa) {
        z12_ = TlsKxShared{};
        z12_.len = 48;
        z12_.z[0] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(TlsProtocolVersion::Tls12) >> 8);
        z12_.z[1] = static_cast<std::uint8_t>(TlsProtocolVersion::Tls12);
        const auto leaf_key = peer_chain_.leaf().public_key();
        std::array<std::uint8_t, kClientMaxRsaCiphertext> encrypted{};
        common::IoResult<std::size_t> encrypted_len = std::unexpected(common::IoErr::Invalid);
        if (tls_random_bytes({z12_.z.data() + 2, 46}) && leaf_key.has_value()) {
            encrypted_len = leaf_key->rsa_encrypt_pkcs1(z12_.bytes(), encrypted);
        }
        if (encrypted_len.has_value()) {
            cke_len = tls_encode_client_key_exchange_rsa({encrypted.data(), *encrypted_len},
                                                         {scratch_.data(), scratch_.size()});
        }
    } else {
        cke_len = tls_encode_client_key_exchange(hello_.kx->public_value().bytes(), {scratch_.data(), scratch_.size()});
    }
    if (!cke_len.has_value() || !t12_.update({scratch_.data(), cke_len.value()}) ||
        !emit_message({scratch_.data(), cke_len.value()})) {
        fail(TlsAlertDesc::InternalError);
        return false;
    }

    // ---- master secret + key_block ----
    // The EMS session_hash is the LIVE transcript at derivation time
    // (BoringSSL ssl_hash_message(CKE) / ssl_add_message_cbb(CKE) both run
    // BEFORE tls1_generate_master_secret): CH..SHD, our own Certificate, AND
    // the CKE — only the still-unwritten CV is excluded.
    if (ems_negotiated_) {
        snapshot12();
        auto master = tls12_extended_master_secret(suite_, z12_.bytes(), {hash_buf_.data(), hash_len()});
        if (!master.has_value()) {
            fail(TlsAlertDesc::InternalError);
            return false;
        }
        master12_ = std::move(master).value();
    } else {
        auto master = tls12_master_secret(suite_, z12_.bytes(), hello_.client_random, server_random_);
        if (!master.has_value()) {
            fail(TlsAlertDesc::InternalError);
            return false;
        }
        master12_ = std::move(master).value();
    }
    auto keys = tls12_key_block(suite_, master12_, hello_.client_random, server_random_);
    if (!keys.has_value()) {
        fail(TlsAlertDesc::InternalError);
        return false;
    }
    kb12_ = keys.value();

    // ---- CertificateVerify over the RAW transcript bytes ----
    // A 1.2 CV signs the concatenated handshake_messages themselves
    // (BoringSSL signs hs->transcript.buffer(); EVP digests per scheme
    // internally — no pre-digested form exists). Sent only when our
    // Certificate was non-empty.
    if (sent_cert12_) {
        std::optional<TlsSignatureScheme> chosen;
        for (const TlsSignatureScheme scheme: kTls12SignaturePreference) {
            if (!cfg_.client_key->supports(scheme, TlsProtocolVersion::Tls12)) {
                continue;
            }
            bool offered_by_cr = !cr_sigalgs_present_; // absent list = no constraint
            for (std::size_t i = 0; !offered_by_cr && i < cr_sigalgs_n_; ++i) {
                offered_by_cr = cr_sigalgs_[i] == static_cast<std::uint16_t>(scheme);
            }
            if (offered_by_cr) {
                chosen = scheme;
                break;
            }
        }
        if (!chosen.has_value()) {
            fail(TlsAlertDesc::HandshakeFailure); // no common scheme
            return false;
        }
        std::array<std::uint8_t, kClientMaxSigLen> sig{};
        FIBER_ASSERT(cfg_.client_key->max_signature_len() <= sig.size());
        const auto sig_len = cfg_.client_key->sign(*chosen, TlsProtocolVersion::Tls12, t12_.buffer(), sig);
        if (!sig_len.has_value()) {
            fail(TlsAlertDesc::InternalError);
            return false;
        }
        const auto cv_len = tls_encode_certificate_verify(
                static_cast<std::uint16_t>(*chosen), {sig.data(), sig_len.value()}, {scratch_.data(), scratch_.size()});
        if (!cv_len.has_value() || !t12_.update({scratch_.data(), cv_len.value()}) ||
            !emit_message({scratch_.data(), cv_len.value()})) {
            fail(TlsAlertDesc::InternalError);
            return false;
        }
    }

    // ---- CCS + write-cipher swap: everything after this is sealed ----
    if (!ctx_.send_ccs().has_value() || !swap_cipher_12(ctx_.write_cipher(), TlsRecordDirection::Seal, kb12_.client)) {
        fail(TlsAlertDesc::InternalError);
        return false;
    }

    // ---- client Finished (sealed; MAC over the pre-Fin transcript) ----
    snapshot12();
    auto verify = tls12_verify_data(suite_, master12_, true, {hash_buf_.data(), hash_len()});
    if (!verify.has_value()) {
        fail(TlsAlertDesc::InternalError);
        return false;
    }
    const auto fin_len = tls_encode_finished({verify->data(), verify->size()}, {scratch_.data(), scratch_.size()});
    if (!fin_len.has_value() || !t12_.update({scratch_.data(), fin_len.value()}) ||
        !emit_message({scratch_.data(), fin_len.value()})) {
        fail(TlsAlertDesc::InternalError);
        return false;
    }
    return true;
}

void Tls12ClientHandshake::handle_new_session_ticket_12(std::span<const std::uint8_t> body) noexcept {
    // RFC 5077 §3.3: lifetime(4) + ticket_len(2) + ticket, no extension block
    // (that is a 1.3 NST). Tickets belong to 08's cache — the handshake merely
    // tolerates (and transcript-hashes) any number before the server's CCS.
    if (body.size() < 6 || (static_cast<std::size_t>(body[4]) << 8 | body[5]) + 6 != body.size()) {
        fail(TlsAlertDesc::DecodeError);
        return;
    }
    feed12(TlsHandshakeType::NewSessionTicket, body);
}

void Tls12ClientHandshake::handle_finished_12(std::span<const std::uint8_t> body) noexcept {
    TlsFinished fin;
    if (!tls_decode_finished(body.data(), body.size(), fin).has_value() || fin.verify_data.size() != 12) {
        fail(TlsAlertDesc::DecodeError); // 1.2 verify_data is always 12 bytes
        return;
    }
    // verify_data MACs the message list through OUR flight (the CCS is not a
    // handshake message) — snapshotted before the server Fin itself is fed.
    snapshot12();
    const auto expected = tls12_verify_data(suite_, master12_, false, {hash_buf_.data(), hash_len()});
    if (!expected.has_value()) {
        fail(TlsAlertDesc::InternalError);
        return;
    }
    if (!tls_constant_time_equal({expected->data(), expected->size()}, fin.verify_data)) {
        fail(TlsAlertDesc::DecryptError);
        return;
    }
    feed12(TlsHandshakeType::Finished, body);
    finish_1_2();
}

void Tls12ClientHandshake::finish_1_2() noexcept {
    state_.version = TlsProtocolVersion::Tls12;
    state_.suite = suite_;
    state_.read_cipher = std::move(ctx_.read_cipher());
    state_.write_cipher = std::move(ctx_.write_cipher());
    state_.tls12_master = std::move(master12_);
    state_.session_resumed = false;
    state_.early_data_accepted = false;
    state_.peer_chain = std::move(peer_chain_);
    wipe_kb12(); // both ciphers hold the key_block material now
    out_.done = true;
    st_ = St::Done;
    // Reader bytes after the server Fin stay buffered — post-handshake
    // messages (tickets under a renegotiated-connection banner) belong to 08.
}

} // namespace fiber::tls
