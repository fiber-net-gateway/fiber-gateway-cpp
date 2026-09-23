#include "Tls13ServerHandshake.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "../crypto/TlsCryptoPrimitives.h"

#include <fiber/tls/TlsVersion.h>
#include <fiber/tls/crypto/TlsCertificate.h>
#include <fiber/tls/crypto/TlsSignature.h>
#include <fiber/tls/handshake/TlsExtensionCodec.h>
#include <fiber/tls/record/TlsRecord.h>

namespace fiber::tls {

namespace {

constexpr std::size_t kServerCvCtxLen = sizeof("TLS 1.3, server CertificateVerify") - 1;
constexpr std::size_t kClientCvCtxLen = sizeof("TLS 1.3, client CertificateVerify") - 1;
constexpr std::size_t kCvContentMax = 64 + kClientCvCtxLen + 1 + 48; // 64 spaces + ctx + NUL + SHA-384

// CertificateVerify content (RFC 8446 §4.4.3) — the mirror of the client's
// file-local builder: 64×0x20, the context string, a zero byte, then the
// transcript snapshot.
void build_cert_verify_content(std::span<std::uint8_t> out, std::span<const char> context,
                               std::span<const std::uint8_t> transcript_hash) noexcept {
    FIBER_ASSERT(out.size() == 64 + context.size() + 1 + transcript_hash.size());
    std::memset(out.data(), 0x20, 64);
    std::memcpy(out.data() + 64, context.data(), context.size());
    out[64 + context.size()] = 0x00;
    std::memcpy(out.data() + 64 + context.size() + 1, transcript_hash.data(), transcript_hash.size());
}

} // namespace

Tls13ServerHandshake::Tls13ServerHandshake(const Mount &mount) noexcept :
    ctx_(mount.ctx), cfg_(mount.cfg), resumption_(mount.resumption), minter_(mount.minter), hello_(mount.hello),
    early_(mount.early), out_(mount.out), scratch_(mount.scratch) {}

Tls13ServerHandshake::~Tls13ServerHandshake() {
    // Explicit wipes at the handoff points the 02 contract names; the
    // schedule and key exchange wipe themselves. Secrets already moved into
    // state_ arrive here moved-from (pre-wiped).
    client_hs_.wipe();
    server_hs_.wipe();
    client_app0_.wipe();
    server_app0_.wipe();
    resumption_master_.wipe();
    client_early_.wipe();
    resumed_psk_.wipe();
}

// =====================================================================
// Entry: ClientHello → parameter selection → the whole server flight
// =====================================================================

void Tls13ServerHandshake::start(const TlsClientHello &ch, std::span<const std::uint8_t> body) noexcept {
    // The shell already rejected a non-null compression method and version
    // mismatches at the fork; what remains 1.3-specific here: the mandatory
    // offers (RFC 8446 §4.2: signature_algorithms, supported_groups,
    // key_share in a full handshake).
    if (!ch.has_signature_algorithms || !ch.has_supported_groups || !ch.has_key_share) {
        fail(TlsAlertDesc::MissingExtension);
        return;
    }

    // The NST mint gate rides the MODES extension alone (RFC 8446 §4.2.9:
    // servers must not send tickets unless psk_dhe_ke was offered — a
    // standing extension every resumable client sends, PSK offer or not).
    psk_dhe_ke_offered_ =
            ch.has_psk_key_exchange_modes && tls_psk_modes_contains(ch.psk_key_exchange_modes, kTlsPskModePskDheKe);

    // ---- PSK cascade (§2.4) — before preference selection: an accepted
    // ticket FIXES the suite (the binder binds it). Structure violations are
    // fatal; every gate miss (miss/age/suite/no-dhe-mode) merely drops the
    // ticket onto the full-handshake path. ----
    if (ch.has_pre_shared_key) {
        TlsAlertDesc alert = TlsAlertDesc::InternalError;
        const PskOutcome psk = try_accept_psk(ch, body, nullptr, alert);
        if (psk == PskOutcome::Fatal) {
            fail(alert);
            return;
        }
    }

    // ---- negotiation (server preference order, §10.2; resumed = ticket suite) ----
    if (!psk_accepted_ && !tls_server_suite_select(ch, true, suite_)) {
        fail(TlsAlertDesc::HandshakeFailure);
        return;
    }
    TlsNamedGroup group = TlsNamedGroup::X25519;
    if (!tls_server_group_select(ch, group)) {
        fail(TlsAlertDesc::HandshakeFailure); // no shared group at all — HRR cannot help
        return;
    }

    // ---- transcript starts over the retained CH (the suite fixes the hash) ----
    if (!t13_.init(suite_info()->hash)) {
        fail(TlsAlertDesc::InternalError);
        return;
    }
    feed13(TlsHandshakeType::ClientHello, body);
    if (psk_accepted_) {
        // Hash(CH) is the client_early_traffic_secret input — snapshot now:
        // the HRR restart below would destroy the running state.
        snapshot13();
        std::memcpy(ch_hash_.data(), hash_buf_.data(), hash_len());
    }

    TlsKeyShareView share;
    const auto found = tls_find_client_key_share(ch.key_share_entries, group, share);
    if (!found.has_value()) {
        fail(TlsAlertDesc::DecodeError);
        return;
    }
    if (!found.value()) {
        // The client supports our preferred group but sent no share for it
        // (RFC 8446 §4.2.8): one HelloRetryRequest asking for it.
        hrr_group_ = group;
        send_hello_retry();
        return;
    }

    // ---- key exchange: the 02c encap's first production consumer ----
    auto kx = TlsKeyExchange::create(group);
    if (!kx.has_value()) {
        fail(TlsAlertDesc::InternalError);
        return;
    }
    const TlsKxShared z = kx.value()->encap(share.key_exchange);
    switch (z.status) {
        case TlsKxStatus::Ok:
            break;
        case TlsKxStatus::BadPeerData:
            fail(TlsAlertDesc::IllegalParameter); // invalid client point
            return;
        case TlsKxStatus::PrimitiveFail:
        default:
            fail(TlsAlertDesc::InternalError);
            return;
    }
    hello_.kx = std::move(kx).value();
    hello_.kx_group = group;

    if (!tls_random_bytes(hello_.server_random)) {
        fail(TlsAlertDesc::InternalError);
        return;
    }
    sched_.emplace(suite_);
    if (psk_accepted_ && !sched_->set_psk(resumed_psk_.bytes()).has_value()) {
        fail(TlsAlertDesc::InternalError);
        return;
    }
    if (psk_accepted_) {
        // The early read secret must leave the schedule while it is still in
        // its Early stage — send_server_flight's handshake_secrets() advances
        // past it long before the 0-RTT read swap runs.
        auto early = sched_->client_early_traffic_secret({ch_hash_.data(), hash_len()});
        if (!early.has_value()) {
            fail(TlsAlertDesc::InternalError);
            return;
        }
        client_early_ = std::move(early).value();
    }
    send_server_flight(ch, {z.z.data(), z.z.size()}, false);
}

// =====================================================================
// PSK cascade (§2.4) and binder verification
// =====================================================================

Tls13ServerHandshake::PskOutcome Tls13ServerHandshake::try_accept_psk(const TlsClientHello &ch,
                                                                      std::span<const std::uint8_t> body,
                                                                      const TlsTranscript13 *base,
                                                                      TlsAlertDesc &alert) noexcept {
    alert = TlsAlertDesc::InternalError;
    if (!ch.has_psk_key_exchange_modes) {
        // pre_shared_key without psk_key_exchange_modes (RFC 8446 §4.2.9).
        alert = TlsAlertDesc::MissingExtension;
        return PskOutcome::Fatal;
    }
    psk_dhe_ke_offered_ = tls_psk_modes_contains(ch.psk_key_exchange_modes, kTlsPskModePskDheKe);

    // pre_shared_key must be the CH's LAST extension and every identity must
    // carry a binder (RFC 8446 §4.2.11). The decoder walks the extension body
    // exactly, so "last" reduces to: the binders vector runs to the body end.
    if (ch.psk_binder_block_offset + 2 + ch.psk_binders.size() != body.size() ||
        ch.psk_identity_count != ch.psk_binder_count) {
        alert = TlsAlertDesc::IllegalParameter;
        return PskOutcome::Fatal;
    }
    if (!psk_dhe_ke_offered_) {
        return PskOutcome::Reject; // psk_ke-only offer: the ticket is ignored, full handshake
    }

    // ---- identity[0] (§2.4: v1 consults exactly the first offer) ----
    TlsPskIdentityView id{};
    const auto has_id = tls_psk_identity_at(ch.psk_identities, 0, id);
    if (!has_id.has_value()) {
        alert = TlsAlertDesc::DecodeError;
        return PskOutcome::Fatal;
    }
    if (!has_id.value() || id.identity.empty() || id.identity.size() > kMaxPskIdentityLen) {
        return PskOutcome::Reject; // no/oversized identity — drop the ticket
    }

    // ---- lookup (hook owns the returned bytes; miss = full handshake). The
    // CH's SNI rides along so the stateless open can check the ticket's AAD
    // name binding, and now_unix_ms is the same clock snapshot the age gate
    // below uses — the lookup's expiry and the gate can never disagree. ----
    TlsResumedSession resumed{};
    if (resumption_ == nullptr || resumption_->lookup == nullptr ||
        !resumption_->lookup(resumption_->ctx, id.identity, hello_.view.server_name, cfg_.now_unix_ms, resumed)) {
        return PskOutcome::Reject;
    }

    // ---- suite gate: the ticket's suite must be a 1.3 suite the CH offered ----
    const TlsSuiteInfo *info = tls_suite_info(resumed.suite);
    if (info == nullptr || !info->is_tls13 ||
        !tls_server_list_contains(ch.cipher_suites, static_cast<std::uint16_t>(resumed.suite))) {
        return PskOutcome::Reject;
    }

    // ---- age gate: |client_age − server_age| ≤ 60 s (§10.4). obfuscated_age
    // = age + ticket_age_add mod 2^32; ticket lifetimes sit far below the
    // wrap horizon, so plain int64 arithmetic suffices. ----
    const std::uint32_t client_age = id.obfuscated_ticket_age - resumed.ticket_age_add;
    const std::uint64_t server_age = cfg_.now_unix_ms > resumed.ticket_issued_ms
                                             ? static_cast<std::uint64_t>(cfg_.now_unix_ms - resumed.ticket_issued_ms)
                                             : 0;
    const std::int64_t delta = static_cast<std::int64_t>(client_age) -
                               static_cast<std::int64_t>(std::min<std::uint64_t>(server_age, 0xFFFFFFFFu));
    const std::int64_t skew = delta < 0 ? -delta : delta;
    if (skew > static_cast<std::int64_t>(kMaxTicketAgeSkewMs)) {
        return PskOutcome::Reject;
    }

    // ---- binder[0]: any mismatch is fatal decrypt_error ----
    if (!verify_psk_binder(resumed.suite, resumed.psk, ch, body, base, alert)) {
        return PskOutcome::Fatal;
    }

    // Accept: retain the resumption material (psk copied — the lookup's
    // bytes live in the hook's caller; everything else re-borrowed below).
    resumed_psk_ = TlsSecret::from_bytes(resumed.psk);
    if (resumed_psk_.empty()) {
        alert = TlsAlertDesc::InternalError;
        return PskOutcome::Fatal;
    }
    resumed_alpn_ = resumed.alpn;
    resumed_age_add_ = resumed.ticket_age_add;
    resumed_max_early_ = resumed.max_early_data;
    suite_ = resumed.suite;
    psk_accepted_ = true;
    return PskOutcome::Accept;
}

bool Tls13ServerHandshake::verify_psk_binder(TlsCipherSuiteId suite, std::span<const std::uint8_t> psk,
                                             const TlsClientHello &ch, std::span<const std::uint8_t> body,
                                             const TlsTranscript13 *base, TlsAlertDesc &alert) noexcept {
    alert = TlsAlertDesc::InternalError;
    std::span<const std::uint8_t> binder{};
    const auto has_binder = tls_psk_binder_at(ch.psk_binders, 0, binder);
    if (!has_binder.has_value()) {
        alert = TlsAlertDesc::DecodeError;
        return false;
    }
    if (!has_binder.value()) {
        alert = TlsAlertDesc::IllegalParameter;
        return false;
    }

    // Local schedule over the TICKET's suite (at CH1 the negotiated suite is
    // not chosen yet; the binder binds the ticket suite): binder_key is
    // once-per-instance, so the connection schedule cannot serve.
    TlsKeySchedule13 verify(suite);
    if (!verify.set_psk(psk).has_value()) {
        return false;
    }
    auto binder_key = verify.binder_key(TlsPskBinderKind::Resumption);
    if (!binder_key.has_value()) {
        return false;
    }
    if (binder.size() != binder_key->len()) {
        alert = TlsAlertDesc::DecryptError; // binder not hash-length
        return false;
    }
    const TlsHashAlgorithm hash = tls_suite_info(suite)->hash;
    const std::size_t hlen = tls_hash_len(hash);

    // Truncated ClientHello hash: the reconstructed 4-byte header plus the
    // body through the binders vector's length prefix (the block offset),
    // riding the pre-CH transcript base when one exists (CH2).
    TlsTranscript13 truncated = base != nullptr ? base->fork() : TlsTranscript13{};
    if (base == nullptr && !truncated.init(hash)) {
        return false;
    }
    const std::array<std::uint8_t, 4> header{static_cast<std::uint8_t>(TlsHandshakeType::ClientHello), 0,
                                             static_cast<std::uint8_t>(body.size() >> 8),
                                             static_cast<std::uint8_t>(body.size())};
    if (!truncated.update(header) || !truncated.update({body.data(), ch.psk_binder_block_offset})) {
        return false;
    }
    std::array<std::uint8_t, 64> digest{};
    if (!truncated.snapshot_digest({digest.data(), hlen})) {
        return false;
    }
    std::array<std::uint8_t, 48> expected{};
    if (!tls13_psk_binder_mac(*binder_key, {digest.data(), hlen}, {expected.data(), hlen}).has_value()) {
        return false;
    }
    if (!tls_constant_time_equal({expected.data(), hlen}, binder)) {
        alert = TlsAlertDesc::DecryptError;
        return false;
    }
    return true;
}

// The HRR wire form: an SH shell with the sentinel random and exactly the
// supported_versions + key_share(selected_group, empty) extensions (no
// cookie — no state to carry). Transcript restarts over MessageHash(Hash(
// CH1)) ‖ HRR ‖ CH2 (RFC 8446 §4.1.4/§4.4.1); a compat CCS follows the HRR.
void Tls13ServerHandshake::send_hello_retry() noexcept {
    if (!t13_.restart_message_hash()) {
        fail(TlsAlertDesc::InternalError);
        return;
    }
    TlsServerHelloInput hrr{};
    hrr.random = kTlsHelloRetryRandom;
    hrr.session_id = hello_.view.session_id;
    hrr.cipher_suite = static_cast<std::uint16_t>(suite_);
    hrr.tls13 = true;
    hrr.key_share_group = static_cast<std::uint16_t>(hrr_group_);
    hrr.key_share = {}; // empty => the HRR selected_group form
    const auto hrr_len = tls_encode_server_hello(hrr, scratch_);
    if (!hrr_len.has_value() || !t13_.update({scratch_.data(), hrr_len.value()}) ||
        !emit_message({scratch_.data(), hrr_len.value()})) {
        fail(TlsAlertDesc::InternalError);
        return;
    }
    if (!ctx_.send_ccs().has_value()) {
        fail(TlsAlertDesc::InternalError);
        return;
    }
    ccs_sent_ = true;
    if (hello_.view.has_early_data) {
        // CH1 offered 0-RTT: its early records are in flight and no key of
        // ours can open them — discard outer application_data by type while
        // CH2 is owed (RFC 8446 §4.2.10); 0-RTT after HRR is dead anyway.
        ctx_.arm_early_data_skip(kMaxEarlyDataSkipped);
    }
    st_ = St::WaitClientHello2;
}

// The second ClientHello (RFC 8446 §4.1.2): the echo fields must equal CH1's
// (compared against the retained hello_.view — CH2's decode borrows the ctx
// body, so both views are live at once), the 1.3 offers must survive, and
// the requested share must now be present. The whole continuation (SH
// flight) rides this handler; CH2 bytes are never retained.
void Tls13ServerHandshake::handle_client_hello2(std::span<const std::uint8_t> body) noexcept {
    TlsClientHello ch2;
    if (!tls_decode_client_hello(body.data(), body.size(), ch2).has_value()) {
        fail(TlsAlertDesc::DecodeError);
        return;
    }
    const TlsClientHello &ch1 = hello_.view;

    // ---- echo-field equality (RFC 8446 §4.1.2: illegal_parameter on any) ----
    const bool echoes =
            ch2.legacy_version == ch1.legacy_version && ch2.random.size() == 32 &&
            0 == std::memcmp(ch2.random.data(), ch1.random.data(), 32) &&
            ch2.session_id.size() == ch1.session_id.size() &&
            0 == std::memcmp(ch2.session_id.data(), ch1.session_id.data(), ch1.session_id.size()) &&
            ch2.cipher_suites.size() == ch1.cipher_suites.size() &&
            0 == std::memcmp(ch2.cipher_suites.data(), ch1.cipher_suites.data(), ch1.cipher_suites.size()) &&
            ch2.compression_methods.size() == ch1.compression_methods.size() &&
            0 == std::memcmp(ch2.compression_methods.data(), ch1.compression_methods.data(),
                             ch1.compression_methods.size());
    if (!echoes) {
        fail(TlsAlertDesc::IllegalParameter);
        return;
    }

    // ---- 1.3 shape: version still offered, early_data withdrawn, PSK
    // re-offered when CH1 offered it, no cookie echo (we sent none; the
    // decoder does not surface cookie, so scan the raw extension block) ----
    bool ch2_has_cookie = false;
    {
        std::size_t off = 0;
        const std::span<const std::uint8_t> ext = ch2.extensions_block;
        while (off + 4 <= ext.size()) {
            const std::uint16_t type =
                    static_cast<std::uint16_t>((static_cast<std::uint16_t>(ext[off]) << 8) | ext[off + 1]);
            const std::uint16_t len =
                    static_cast<std::uint16_t>((static_cast<std::uint16_t>(ext[off + 2]) << 8) | ext[off + 3]);
            if (off + 4 + len > ext.size()) {
                break;
            }
            if (type == 44) { // cookie (RFC 8446 §4.2.2)
                ch2_has_cookie = true;
                break;
            }
            off += 4 + len;
        }
    }
    if (!ch2.has_supported_versions || !tls_server_list_contains(ch2.supported_versions, kTlsVersionTls13) ||
        ch2.has_early_data || ch2_has_cookie || !ch2.has_key_share || !ch2.has_signature_algorithms ||
        (ch1.has_pre_shared_key && !ch2.has_pre_shared_key)) {
        fail(TlsAlertDesc::IllegalParameter);
        return;
    }
    if (ch2.has_pre_shared_key && !ch2.has_psk_key_exchange_modes) {
        fail(TlsAlertDesc::MissingExtension); // RFC 8446 §4.1.2/§4.2.9
        return;
    }
    // The mint gate re-reads CH2's modes (RFC 8446 §4.1.2: a CH2 drops
    // extensions CH1 lacked, so the final offer is what counts).
    psk_dhe_ke_offered_ =
            ch2.has_psk_key_exchange_modes && tls_psk_modes_contains(ch2.psk_key_exchange_modes, kTlsPskModePskDheKe);
    if (psk_accepted_) {
        // The PSK decision from CH1 is immutable; CH2 owes a fresh binder[0]
        // over the restarted transcript (message_hash‖HRR prefix) — forked
        // off t13_ BEFORE CH2 itself is fed.
        TlsAlertDesc alert = TlsAlertDesc::InternalError;
        const TlsTranscript13 base = t13_.fork();
        if (!verify_psk_binder(suite_, resumed_psk_.bytes(), ch2, body, &base, alert)) {
            fail(alert);
            return;
        }
    }

    // ---- the requested share must now exist; a second miss is final ----
    TlsKeyShareView share;
    const auto found = tls_find_client_key_share(ch2.key_share_entries, hrr_group_, share);
    if (!found.has_value()) {
        fail(TlsAlertDesc::DecodeError);
        return;
    }
    if (!found.value()) {
        fail(TlsAlertDesc::UnexpectedMessage); // RFC 8446 §4.2.8: no second HRR
        return;
    }

    feed13(TlsHandshakeType::ClientHello, body);

    auto kx = TlsKeyExchange::create(hrr_group_);
    if (!kx.has_value()) {
        fail(TlsAlertDesc::InternalError);
        return;
    }
    const TlsKxShared z = kx.value()->encap(share.key_exchange);
    switch (z.status) {
        case TlsKxStatus::Ok:
            break;
        case TlsKxStatus::BadPeerData:
            fail(TlsAlertDesc::IllegalParameter);
            return;
        case TlsKxStatus::PrimitiveFail:
        default:
            fail(TlsAlertDesc::InternalError);
            return;
    }
    hello_.kx = std::move(kx).value();
    hello_.kx_group = hrr_group_;

    if (!tls_random_bytes(hello_.server_random)) {
        fail(TlsAlertDesc::InternalError);
        return;
    }
    sched_.emplace(suite_);
    if (psk_accepted_ && !sched_->set_psk(resumed_psk_.bytes()).has_value()) {
        fail(TlsAlertDesc::InternalError);
        return;
    }
    send_server_flight(ch2, {z.z.data(), z.z.size()}, true);
}

// SH + compat CCS + EE + [Cert/CV] + Fin — everything up to (and including)
// the inbound-mode setup (07 §4.2 SendServerHello/SendFlight rows). `z` is
// the (EC)DHE shared secret from start()'s encap. A resumed flight (PSK
// accepted) drops CertificateRequest/Certificate/CertificateVerify — the PSK
// IS the authentication (RFC 8446 §4.2) — and echoes pre_shared_key in the
// SH; `after_hrr` marks the CH2 entry, where 0-RTT is dead by definition.
void Tls13ServerHandshake::send_server_flight(const TlsClientHello &ch, std::span<const std::uint8_t> z,
                                              bool after_hrr) noexcept {
    // ---- ServerHello (plaintext record; session_id echoed) ----
    TlsServerHelloInput sh{};
    sh.random = hello_.server_random;
    sh.session_id = ch.session_id;
    sh.cipher_suite = static_cast<std::uint16_t>(suite_);
    sh.tls13 = true;
    sh.key_share_group = static_cast<std::uint16_t>(hello_.kx_group);
    sh.key_share = hello_.kx->public_value().bytes();
    sh.selected_identity = psk_accepted_; // pre_shared_key echo, always identity 0, LAST extension
    const auto sh_len = tls_encode_server_hello(sh, scratch_);
    if (!sh_len.has_value() || !t13_.update({scratch_.data(), sh_len.value()}) ||
        !emit_message({scratch_.data(), sh_len.value()})) {
        fail(TlsAlertDesc::InternalError);
        return;
    }

    // ---- handshake secrets over Hash(CH..SH); the write cipher goes live
    // (the flight below is sealed); the read swap waits until the end — its
    // direction depends on the 0-RTT decision, which needs the ALPN result ----
    snapshot13();
    if (!sched_->handshake_secrets(z, {hash_buf_.data(), hash_len()}, client_hs_, server_hs_) ||
        !swap_cipher(ctx_.write_cipher(), server_hs_)) {
        fail(TlsAlertDesc::InternalError);
        return;
    }
    ctx_.set_inbound_mode(TlsInboundMode::Sealed13);

    // Compat CCS (RFC 8446 D.4): plaintext, between the SH and the sealed
    // flight; not transcript-fed. The server sends exactly one per
    // handshake — the HRR path already spent it.
    if (!ccs_sent_ && !ctx_.send_ccs().has_value()) {
        fail(TlsAlertDesc::InternalError);
        return;
    }
    ccs_sent_ = true;

    // ---- ALPN + the 0-RTT accept decision (§2.4): the master switch ∧ PSK
    // accepted ∧ CH offered early_data ∧ the ticket allows it ∧ no HRR ∧ the
    // negotiated protocol equals the ticket's early_alpn ----
    std::string_view alpn{};
    const TlsServerAlpnResult alpn_result = tls_server_alpn_select(cfg_, ch, alpn);
    if (alpn_result == TlsServerAlpnResult::Failed) {
        fail(TlsAlertDesc::NoApplicationProtocol);
        return;
    }
    const std::string_view negotiated = alpn_result == TlsServerAlpnResult::Matched ? alpn : std::string_view{};
    early_accepted_ = cfg_.enable_early_data && psk_accepted_ && ch.has_early_data && resumed_max_early_ > 0 &&
                      !after_hrr && negotiated == resumed_alpn_;

    // ---- EncryptedExtensions ----
    TlsEncryptedExtensionsInput ee{};
    ee.acknowledge_server_name = ch.has_server_name;
    ee.alpn = negotiated;
    ee.early_data = early_accepted_;
    if (alpn_result == TlsServerAlpnResult::Matched) {
        FIBER_ASSERT(ee.alpn.size() <= state_.alpn.size());
        std::memcpy(state_.alpn.data(), ee.alpn.data(), ee.alpn.size());
        state_.alpn_len = static_cast<std::uint16_t>(ee.alpn.size());
    }
    const auto ee_len = tls_encode_encrypted_extensions(ee, scratch_);
    if (!ee_len.has_value() || !t13_.update({scratch_.data(), ee_len.value()}) ||
        !emit_message({scratch_.data(), ee_len.value()})) {
        fail(TlsAlertDesc::InternalError);
        return;
    }

    if (!psk_accepted_) {
        // ---- full-handshake authentication flight ----
        // [CertificateRequest]: mTLS only (client_trust non-null = request)
        if (cfg_.client_trust != nullptr) {
            const auto cr_len = tls_encode_certificate_request_13(kServerCrSigalgs, scratch_);
            if (!cr_len.has_value() || !t13_.update({scratch_.data(), cr_len.value()}) ||
                !emit_message({scratch_.data(), cr_len.value()})) {
                fail(TlsAlertDesc::InternalError);
                return;
            }
        }

        // ---- Certificate: the chain's DERs, zero-copy re-send ----
        const TlsCertificateChain &chain = *cfg_.chain;
        std::array<std::span<const std::uint8_t>, TlsCertificateChain::kMaxCerts> ders{};
        ders[0] = chain.leaf().der();
        for (std::size_t i = 0; i < chain.intermediates().size(); ++i) {
            ders[i + 1] = chain.intermediates()[i].der();
        }
        const auto cert_len = tls_encode_certificate_13({}, {ders.data(), chain.size()}, scratch_);
        if (!cert_len.has_value() || !t13_.update({scratch_.data(), cert_len.value()}) ||
            !emit_message({scratch_.data(), cert_len.value()})) {
            fail(TlsAlertDesc::InternalError);
            return;
        }

        // ---- CertificateVerify over the transcript through Cert ----
        snapshot13();
        TlsSignatureScheme scheme = TlsSignatureScheme::RsaPssRsaeSha256;
        if (!tls_server_cv_scheme_select(*cfg_.key, ch, TlsProtocolVersion::Tls13, scheme)) {
            fail(TlsAlertDesc::HandshakeFailure); // no scheme both sides accept for this key
            return;
        }
        std::array<std::uint8_t, kCvContentMax> content{};
        const std::size_t content_len = 64 + kServerCvCtxLen + 1 + hash_len();
        build_cert_verify_content({content.data(), content_len}, {"TLS 1.3, server CertificateVerify", kServerCvCtxLen},
                                  {hash_buf_.data(), hash_len()});
        std::array<std::uint8_t, kServerMaxSigLen> sig{};
        FIBER_ASSERT(cfg_.key->max_signature_len() <= sig.size());
        const auto sig_len = cfg_.key->sign(scheme, TlsProtocolVersion::Tls13, {content.data(), content_len}, sig);
        if (!sig_len.has_value()) {
            fail(TlsAlertDesc::InternalError);
            return;
        }
        const auto cv_len = tls_encode_certificate_verify(static_cast<std::uint16_t>(scheme),
                                                          {sig.data(), sig_len.value()}, scratch_);
        if (!cv_len.has_value() || !t13_.update({scratch_.data(), cv_len.value()}) ||
            !emit_message({scratch_.data(), cv_len.value()})) {
            fail(TlsAlertDesc::InternalError);
            return;
        }
    }

    // ---- Finished over the transcript through CV (full) or EE (resumed) ----
    snapshot13();
    std::array<std::uint8_t, 48> verify_data{};
    if (!tls13_finished_mac(server_hs_, {hash_buf_.data(), hash_len()}, {verify_data.data(), hash_len()}).has_value()) {
        fail(TlsAlertDesc::InternalError);
        return;
    }
    const auto fin_len = tls_encode_finished({verify_data.data(), hash_len()}, scratch_);
    if (!fin_len.has_value() || !t13_.update({scratch_.data(), fin_len.value()}) ||
        !emit_message({scratch_.data(), fin_len.value()})) {
        fail(TlsAlertDesc::InternalError);
        return;
    }

    // ---- application secrets over Hash(CH..server Fin); server_app0 write ----
    snapshot13();
    if (!sched_->application_secrets({hash_buf_.data(), hash_len()}, client_app0_, server_app0_) ||
        !swap_cipher(ctx_.write_cipher(), server_app0_)) {
        fail(TlsAlertDesc::InternalError);
        return;
    }

    // ---- read side: the client flight arrives sealed under either the
    // early keys (0-RTT accepted — EndOfEarlyData swaps in client_hs_) or
    // client_hs_ from the start ----
    if (early_accepted_) {
        if (!swap_cipher(ctx_.read_cipher(), client_early_)) {
            fail(TlsAlertDesc::InternalError);
            return;
        }
        ctx_.arm_early_data_sink(early_, kMaxEarlyDataAccepted);
        st_ = St::WaitEndOfEarlyData;
        return;
    }
    if (!swap_cipher(ctx_.read_cipher(), client_hs_)) {
        fail(TlsAlertDesc::InternalError);
        return;
    }
    if (ch.has_early_data) {
        // 0-RTT was offered but not accepted: the client's early records are
        // in flight sealed under keys we never derived — trial-open with
        // client_hs_ and discard failures (§4.2.10).
        ctx_.arm_early_data_skip(kMaxEarlyDataSkipped);
    }
    // Both entry paths land here: the direct CH1 path and the HRR path
    // (advancing out of WaitClientHello2); mTLS widens the wait with the
    // client's Cert/CV pair (never in a resumed flow — no CR was sent).
    st_ = cfg_.client_trust != nullptr && !psk_accepted_ ? St::WaitClientCert : St::WaitClientFin;
}

// =====================================================================
// Client flight
// =====================================================================

void Tls13ServerHandshake::on_message(TlsHandshakeType type, std::span<const std::uint8_t> body) noexcept {
    if (st_ == St::Done) {
        return; // post-handshake traffic belongs to 08/09 (KeyUpdate, NST ack)
    }
    if (st_ == St::WaitClientHello2) {
        if (type != TlsHandshakeType::ClientHello) {
            fail(TlsAlertDesc::UnexpectedMessage);
            return;
        }
        handle_client_hello2(body);
        return;
    }
    if (st_ == St::WaitClientCert) {
        if (type != TlsHandshakeType::Certificate) {
            fail(TlsAlertDesc::UnexpectedMessage);
            return;
        }
        handle_client_certificate_13(body);
        return;
    }
    if (st_ == St::WaitClientCv) {
        if (type != TlsHandshakeType::CertificateVerify) {
            fail(TlsAlertDesc::UnexpectedMessage);
            return;
        }
        handle_client_certificate_verify_13(body);
        return;
    }
    if (st_ == St::WaitEndOfEarlyData) {
        // 0-RTT accepted: the client's second flight opens with EOED (inner
        // app data kept sinking until it arrives). RFC 8446 §4.5: the EOED
        // record itself is sealed under the EARLY keys — which is why the
        // read cipher is still the early instance here (no dual-cipher trial
        // on the server side).
        if (type != TlsHandshakeType::EndOfEarlyData) {
            fail(TlsAlertDesc::UnexpectedMessage);
            return;
        }
        handle_end_of_early_data(body);
        return;
    }
    if (st_ == St::WaitClientFin) {
        // The client flight ends with Finished.
        if (type != TlsHandshakeType::Finished) {
            fail(TlsAlertDesc::UnexpectedMessage);
            return;
        }
        handle_client_finished(body);
        return;
    }
    fail(TlsAlertDesc::UnexpectedMessage);
}

// =====================================================================
// mTLS: the client's Certificate + CertificateVerify
// =====================================================================

void Tls13ServerHandshake::handle_client_certificate_13(std::span<const std::uint8_t> body) noexcept {
    TlsCertificate13 cert;
    if (!tls_decode_certificate_13(body.data(), body.size(), cert).has_value()) {
        fail(TlsAlertDesc::DecodeError);
        return;
    }
    // We requested with an empty context; the echo must match.
    if (!cert.certificate_request_context.empty()) {
        fail(TlsAlertDesc::IllegalParameter);
        return;
    }
    if (cert.cert_count == 0) {
        // An empty chain is structurally valid (07 §3.2): fatal only when the
        // config requires a client certificate.
        if (cfg_.require_client_cert) {
            fail(TlsAlertDesc::CertificateRequired);
            return;
        }
        feed13(TlsHandshakeType::Certificate, body);
        st_ = St::WaitClientFin; // no credential — no CertificateVerify
        return;
    }
    auto chain = TlsCertificateChain::from_der_list({cert.certs, cert.cert_count});
    if (!chain.has_value()) {
        fail(TlsAlertDesc::DecodeError);
        return;
    }
    peer_chain_ = std::move(chain).value();
    feed13(TlsHandshakeType::Certificate, body);

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
    st_ = St::WaitClientCv;
}

void Tls13ServerHandshake::handle_client_certificate_verify_13(std::span<const std::uint8_t> body) noexcept {
    TlsCertificateVerify cv;
    if (!tls_decode_certificate_verify(body.data(), body.size(), cv).has_value()) {
        fail(TlsAlertDesc::DecodeError);
        return;
    }
    // The scheme must be one we offered in the CertificateRequest.
    bool offered = false;
    for (const TlsSignatureScheme scheme: kTls13SignaturePreference) {
        if (static_cast<std::uint16_t>(scheme) == cv.algorithm) {
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
    // The signature covers the transcript through the client's Certificate —
    // snapshotted BEFORE this message itself is fed.
    snapshot13();
    std::array<std::uint8_t, kCvContentMax> content{};
    const std::size_t content_len = 64 + kClientCvCtxLen + 1 + hash_len();
    build_cert_verify_content({content.data(), content_len}, {"TLS 1.3, client CertificateVerify", kClientCvCtxLen},
                              {hash_buf_.data(), hash_len()});
    const auto valid = tls_verify(static_cast<TlsSignatureScheme>(cv.algorithm), TlsProtocolVersion::Tls13, *public_key,
                                  {content.data(), content_len}, cv.signature);
    if (!valid.has_value()) {
        fail(TlsAlertDesc::IllegalParameter); // scheme does not match the client's key
        return;
    }
    if (!valid.value()) {
        fail(TlsAlertDesc::DecryptError);
        return;
    }
    feed13(TlsHandshakeType::CertificateVerify, body);
    st_ = St::WaitClientFin;
}

// RFC 8446 §4.5: EndOfEarlyData has an empty body by construction (a
// non-empty body is a decode_error). It is fed into the transcript — the
// client's Finished covers it — then the 0-RTT window closes: sink disarmed,
// read swapped to a fresh client_hs_ instance (new key, sequence restarts at
// zero), and the flight ends with the client's Finished as always.
void Tls13ServerHandshake::handle_end_of_early_data(std::span<const std::uint8_t> body) noexcept {
    if (!body.empty()) {
        fail(TlsAlertDesc::DecodeError);
        return;
    }
    feed13(TlsHandshakeType::EndOfEarlyData, body);
    ctx_.disarm_early_data();
    if (!swap_cipher(ctx_.read_cipher(), client_hs_)) {
        fail(TlsAlertDesc::InternalError);
        return;
    }
    st_ = St::WaitClientFin;
}

void Tls13ServerHandshake::handle_client_finished(std::span<const std::uint8_t> body) noexcept {
    TlsFinished fin;
    if (!tls_decode_finished(body.data(), body.size(), fin).has_value() || fin.verify_data.size() != hash_len()) {
        fail(TlsAlertDesc::DecodeError);
        return;
    }
    // verify_data covers the transcript through whatever preceded Fin —
    // snapshotted before Fin itself is fed.
    snapshot13();
    std::array<std::uint8_t, 48> expected{};
    if (!tls13_finished_mac(client_hs_, {hash_buf_.data(), hash_len()}, {expected.data(), hash_len()}).has_value()) {
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

void Tls13ServerHandshake::finish_1_3() noexcept {
    // Resumption base over Hash(CH..client Fin) — after feeding it — then the
    // read side swaps to a fresh client_app0 instance; the hs instance dies
    // with the flight.
    snapshot13();
    auto resumption = sched_->resumption_master_secret({hash_buf_.data(), hash_len()});
    if (!resumption.has_value() || !swap_cipher(ctx_.read_cipher(), client_app0_)) {
        fail(TlsAlertDesc::InternalError);
        return;
    }
    resumption_master_ = std::move(resumption).value();

    // ---- NewSessionTicket (§10.9): exactly one ticket, nonce 0, only when
    // the CH offered psk_dhe_ke (the only mode a resumption PSK may use) and
    // the glue installed a minter. Post-handshake: NOT transcript-fed,
    // sealed under the live server_app0 write cipher, deferred to the record
    // stream like every server emission. The ticket blob mints into the
    // scratch's back half; the NST encodes into the front. ----
    if (psk_dhe_ke_offered_ && minter_ != nullptr && minter_->mint != nullptr) {
        std::array<std::uint8_t, 4> age_add{};
        if (!tls_random_bytes(age_add)) {
            fail(TlsAlertDesc::InternalError);
            return;
        }
        const std::uint32_t age_add_v = static_cast<std::uint32_t>(static_cast<std::uint32_t>(age_add[0]) << 24) |
                                        (static_cast<std::uint32_t>(age_add[1]) << 16) |
                                        (static_cast<std::uint32_t>(age_add[2]) << 8) |
                                        static_cast<std::uint32_t>(age_add[3]);
        const std::size_t cap = scratch_.size() / 2;
        TlsTicketRequest request{};
        request.resumption_master = resumption_master_.bytes();
        request.ticket_nonce = 0;
        request.suite = suite_;
        request.alpn = std::string_view{reinterpret_cast<const char *>(state_.alpn.data()), state_.alpn_len};
        request.ticket_age_add = age_add_v;
        request.max_early_data = cfg_.enable_early_data ? kMaxEarlyDataAccepted : 0;
        request.timeout_s = cfg_.session_timeout_s;
        request.now_unix_ms = cfg_.now_unix_ms;
        request.version = TlsProtocolVersion::Tls13;
        // The CH's SNI rides the retained copy (hello_.ch) — stable storage,
        // and the minter call is synchronous, so the borrow is sound.
        request.name = hello_.view.server_name;
        const std::size_t ticket_len = minter_->mint(minter_->ctx, request, {scratch_.data() + cap, cap});
        if (ticket_len == 0) {
            // The hook declined (e.g. no session store) — no NST this connection.
        } else if (ticket_len > cap) {
            fail(TlsAlertDesc::InternalError); // hook wrote out of bounds territory
            return;
        } else {
            TlsNewSessionTicket13Input nst{};
            nst.lifetime_s = cfg_.session_timeout_s;
            nst.ticket_age_add = age_add_v;
            nst.ticket_nonce = 0;
            nst.ticket = {scratch_.data() + cap, ticket_len};
            nst.max_early_data = cfg_.enable_early_data ? kMaxEarlyDataAccepted : 0;
            const auto nst_len = tls_encode_new_session_ticket_13(nst, {scratch_.data(), scratch_.size()});
            if (!nst_len.has_value() || !emit_message({scratch_.data(), nst_len.value()})) {
                fail(TlsAlertDesc::InternalError);
                return;
            }
        }
    }

    // ---- Done ----
    state_.version = TlsProtocolVersion::Tls13;
    state_.suite = suite_;
    state_.read_cipher = std::move(ctx_.read_cipher());
    state_.write_cipher = std::move(ctx_.write_cipher());
    state_.client_app_secret = std::move(client_app0_);
    state_.server_app_secret = std::move(server_app0_);
    state_.resumption_master = std::move(resumption_master_);
    state_.session_resumed = psk_accepted_;
    state_.early_data_accepted = early_accepted_;
    state_.peer_chain = std::move(peer_chain_);
    out_.done = true;
    st_ = St::Done;
    // Reader bytes arriving after the client Fin stay buffered —
    // post-handshake messages belong to 08/09.
}

} // namespace fiber::tls
