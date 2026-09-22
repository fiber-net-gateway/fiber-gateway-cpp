#include <fiber/tls/handshake/TlsClientHandshakeEngine.h>

#include <array>
#include <cstring>
#include <optional>
#include <span>
#include <utility>

#include "../crypto/TlsCryptoPrimitives.h"
#include "../detail/TlsHandshakeContext.h"
#include "TlsTranscript.h"

#include <fiber/tls/TlsVersion.h>
#include <fiber/tls/crypto/TlsCertificate.h>
#include <fiber/tls/crypto/TlsKeyExchange.h>
#include <fiber/tls/crypto/TlsKeySchedule.h>
#include <fiber/tls/handshake/TlsExtensionCodec.h>
#include <fiber/tls/handshake/TlsHandshakeCodec.h>
#include <fiber/tls/record/TlsRecord.h>

namespace fiber::tls {

namespace {

// ---- engine-fixed offer tables (06 §5.4: registry constants, not config) ----

// 1.3 suites first (preference order), then the ECDHE+AEAD 1.2 set the
// supported_versions fallback can negotiate (0xC030 = ECDHE-RSA-AES256-GCM
// — the IANA value; 0x0030 was never a suite).
constexpr std::array<std::uint16_t, 9> kOfferedSuites{
        0x1301, 0x1302, 0x1303, 0xC02F, 0xC030, 0xCCA8, 0xC02B, 0xC02C, 0xCCA9,
};

// Share order: X25519 leads (the CH1 share); P-256 is the HRR alternative.
constexpr std::array<std::uint16_t, 2> kOfferedGroups{0x001D, 0x0017};

// The CH signature_algorithms offer = the 02b 1.2 preference (a superset:
// rsa_pkcs1_* only negotiates in 1.2; the 1.3 verify path gates by version).
constexpr auto kOfferedSigalgs = [] {
    std::array<std::uint16_t, kTls12SignaturePreference.size()> out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::uint16_t>(kTls12SignaturePreference[i]);
    }
    return out;
}();

constexpr std::size_t kServerCvCtxLen = sizeof("TLS 1.3, server CertificateVerify") - 1;
constexpr std::size_t kClientCvCtxLen = sizeof("TLS 1.3, client CertificateVerify") - 1;
constexpr std::size_t kCvContentMax = 64 + kClientCvCtxLen + 1 + 48; // 64 spaces + ctx + NUL + SHA-384

std::size_t suite_offer_index(std::uint16_t raw) noexcept {
    for (std::size_t i = 0; i < kOfferedSuites.size(); ++i) {
        if (kOfferedSuites[i] == raw) {
            return i;
        }
    }
    return kOfferedSuites.size();
}

bool group_offered(std::uint16_t raw) noexcept { return raw == kOfferedGroups[0] || raw == kOfferedGroups[1]; }

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

// =====================================================================
// Impl
// =====================================================================

struct TlsClientHandshakeEngine::Impl {
    static constexpr std::size_t kChCap = 8192; // retained ClientHello bytes
    static constexpr std::size_t kScratchCap = 32768; // client-flight staging (mTLS chains ≪ 32 KiB)
    static constexpr std::size_t kMaxCrSigalgs = 16;
    static constexpr std::size_t kMaxSigLen = 1024; // RSA-4096 signature bound

    enum class St : std::uint8_t {
        WaitServerHello,
        ExpectEe,
        ExpectCrCertFin,
        ExpectCv,
        ExpectFin,
        // TLS 1.2 sub-flow (06 §4.3): SH(1.2) → Cert → SKE → [CertReq] → SHD →
        // client flight → [NST] → server CCS → server Fin.
        ExpectServerCert12,
        ExpectSke12,
        ExpectCrShd12,
        ExpectServerCcs12,
        ExpectServerFin12,
        Done,
    };

    Impl(const TlsClientConfig &config, const TlsSessionOffer *session_offer) noexcept :
        cfg(config), session(session_offer) {}

    ~Impl() {
        // Explicit wipes at the handoff points the 02 contract names; the
        // schedule and key exchange wipe themselves. Secrets already moved
        // into TlsConnectedState arrive here moved-from (pre-wiped).
        client_hs.wipe();
        server_hs.wipe();
        client_app0.wipe();
        server_app0.wipe();
        resumption_master.wipe();
        master12.wipe();
        wipe_kb12();
        tls_secure_wipe(z12.z.data(), z12.z.size());
    }

    // ---- inputs (borrowed; the net glue outlives the engine) ----
    TlsClientConfig cfg;
    const TlsSessionOffer *session;

    // ---- pipeline + transcripts ----
    TlsHandshakeContext ctx;
    TlsTranscript13 t13;
    TlsTranscript12 t12; // fed once the version is known to be 1.2 (P4)

    // ---- retained ClientHello ----
    std::array<std::uint8_t, kChCap> ch{};
    std::size_t ch_len = 0;
    std::size_t binder_off = 0; // message-relative binder-block offset (0 = no PSK)
    TlsNamedGroup kx_group = TlsNamedGroup::X25519; // group of the share in `ch`
    std::array<std::uint8_t, 32> client_random{};
    std::array<std::uint8_t, 32> session_id{};

    // ---- crypto state ----
    std::optional<TlsKeyExchange> kx;
    std::optional<TlsKeySchedule13> sched;
    TlsSecret client_hs{};
    TlsSecret server_hs{};
    TlsSecret client_app0{};
    TlsSecret server_app0{};
    TlsSecret resumption_master{};
    TlsRecordCipher early_write;
    TlsCipherSuiteId suite = TlsCipherSuiteId::TlsAes128GcmSha256;

    // ---- flow state ----
    St st = St::WaitServerHello;
    std::uint8_t hrr_count = 0;
    TlsCipherSuiteId hrr_suite = TlsCipherSuiteId::TlsAes128GcmSha256; // valid when hrr_count == 1
    bool psk_offered = false;
    bool psk_accepted = false;
    bool sent_early_ext = false; // early_data extension is in the CH
    bool ccs_sent = false; // the one compat CCS went out (RFC 8446 D.4)
    bool early_window_closed = false; // write_early_data() is no longer valid
    bool ee_early_data = false;
    bool cr13_received = false;
    // ---- 1.2 sub-flow ----
    std::array<std::uint8_t, 32> server_random{}; // copied at the SH(1.2) read point
    TlsKxShared z12{};
    TlsSecret master12{};
    Tls12WriteKeys kb12{}; // key_block; wiped once both ciphers hold the material
    bool ems_negotiated = false;
    bool cr12_received = false;
    bool sent_cert12 = false; // we sent a (non-empty) client Certificate
    std::size_t early_written = 0;
    std::array<std::uint16_t, kMaxCrSigalgs> cr_sigalgs{};
    std::size_t cr_sigalgs_n = 0;
    std::array<std::uint8_t, 255> cr_context{};
    std::size_t cr_ctx_len = 0;
    TlsCertificateChain peer_chain;

    // ---- staging + terminal state ----
    std::array<std::uint8_t, kScratchCap> scratch{};
    std::array<std::uint8_t, 64> hash_buf{};
    bool done = false;
    bool failed = false;
    TlsAlertDesc failure_alert = TlsAlertDesc::InternalError;
    TlsConnectedState state{};

    // ---- helpers ----

    [[nodiscard]] const TlsSuiteInfo *suite_info() const noexcept { return tls_suite_info(suite); }

    [[nodiscard]] std::size_t hash_len() const noexcept { return tls_hash_len(suite_info()->hash); }

    void fail_local(TlsAlertDesc alert) noexcept {
        if (done) {
            return;
        }
        ctx.fail(alert); // encodes the fatal alert (sealed when the write cipher is live)
        failed = true;
        done = true;
        failure_alert = alert;
        st = St::Done;
    }

    void fail_peer(TlsAlertDesc alert) noexcept {
        if (done) {
            return;
        }
        failed = true;
        done = true;
        failure_alert = alert; // the peer's alert; nothing is sent back
        st = St::Done;
    }

    void feed13(TlsHandshakeType type, std::span<const std::uint8_t> body) noexcept {
        const std::array<std::uint8_t, 4> header{static_cast<std::uint8_t>(type), 0,
                                                 static_cast<std::uint8_t>(body.size() >> 8),
                                                 static_cast<std::uint8_t>(body.size())};
        (void) t13.update(header);
        (void) t13.update(body);
    }

    void snapshot13() noexcept { (void) t13.snapshot_digest({hash_buf.data(), hash_len()}); }

    void feed12(TlsHandshakeType type, std::span<const std::uint8_t> body) noexcept {
        const std::array<std::uint8_t, 4> header{static_cast<std::uint8_t>(type), 0,
                                                 static_cast<std::uint8_t>(body.size() >> 8),
                                                 static_cast<std::uint8_t>(body.size())};
        (void) t12.update(header);
        (void) t12.update(body);
    }

    void snapshot12() noexcept { (void) t12.snapshot_digest({hash_buf.data(), hash_len()}); }

    // Emits a fully-encoded handshake message (header+body in `msg`); the
    // context seals it when the write cipher is live. False = connection-level
    // failure already encoded.
    [[nodiscard]] bool emit_message(std::span<const std::uint8_t> msg) noexcept {
        return ctx.emit(TlsContentType::Handshake, msg).has_value();
    }

    // Derives traffic keys from `secret` and swaps a FRESH cipher instance
    // into `slot` — re-init of a live instance is a 05 contract violation;
    // every key change is an instance swap (the 1.3 app instances restart at
    // seq 0 while the hs instances' sequence dies with the flight).
    [[nodiscard]] bool swap_cipher(TlsRecordCipher &slot, const TlsSecret &secret) noexcept {
        TlsRecordCipher fresh;
        auto derived = tls13_traffic_keys(secret, suite);
        if (!derived.has_value()) {
            return false;
        }
        TlsTrafficKeys &keys = *derived;
        if (!fresh.init(suite, TlsRecordProtectionKind::Tls13, {keys.key.data(), keys.key_len},
                        {keys.iv.data(), keys.iv_len})
                     .has_value()) {
            return false;
        }
        tls_secure_wipe(keys.key.data(), keys.key.size());
        tls_secure_wipe(keys.iv.data(), keys.iv.size());
        slot = std::move(fresh);
        return true;
    }

    // The 1.2 key_block handoff: a fresh cipher instance over one direction's
    // sliced material (write side at our CCS, read side at the server's). The
    // cipher copies what it needs; kb12 is wiped once BOTH directions hold it.
    [[nodiscard]] bool swap_cipher_12(TlsRecordCipher &slot, const TlsTrafficKeys &keys) noexcept {
        TlsRecordCipher fresh;
        if (!fresh.init(suite, TlsRecordProtectionKind::Tls12, {keys.key.data(), keys.key_len},
                        {keys.iv.data(), keys.iv_len})
                     .has_value()) {
            return false;
        }
        slot = std::move(fresh);
        return true;
    }

    void wipe_kb12() noexcept {
        tls_secure_wipe(kb12.client.key.data(), kb12.client.key.size());
        tls_secure_wipe(kb12.client.iv.data(), kb12.client.iv.size());
        tls_secure_wipe(kb12.server.key.data(), kb12.server.key.size());
        tls_secure_wipe(kb12.server.iv.data(), kb12.server.iv.size());
    }

    // The SKE/CV scheme must be one the CH offered (kOfferedSigalgs is a
    // superset of every 1.2- and 1.3-negotiable scheme).
    [[nodiscard]] bool sigalg_offered(std::uint16_t raw) const noexcept {
        for (const std::uint16_t scheme: kOfferedSigalgs) {
            if (scheme == raw) {
                return true;
            }
        }
        return false;
    }

    // ---- ClientHello construction ----

    [[nodiscard]] bool build_client_hello(bool second, std::uint16_t share_group,
                                          std::span<const std::uint8_t> cookie) noexcept {
        if (!second) {
            // CH1 draws fresh entropy; CH2 keeps CH1's random and session_id
            // (RFC 8446 §4.1.4 — only key_share/cookie change).
            if (!tls_random_bytes(client_random) || !tls_random_bytes(session_id)) {
                return false;
            }
        }

        kx.reset();
        kx.emplace(static_cast<TlsNamedGroup>(share_group));
        kx_group = static_cast<TlsNamedGroup>(share_group);
        if (!kx->generate().has_value()) {
            return false;
        }

        TlsClientHelloInput in{};
        in.random = client_random;
        in.session_id = session_id;
        in.cipher_suites = kOfferedSuites;
        in.supported_groups = kOfferedGroups;
        in.signature_algorithms = kOfferedSigalgs;
        in.key_share_group = share_group;
        in.key_share = kx->public_value().bytes();
        in.cookie = cookie;
        if (cfg.verify_ip.empty()) {
            in.sni_host = cfg.sni_host;
        }
        in.alpn = cfg.alpn;
        if (psk_offered) {
            const TlsSuiteInfo *psk_info = tls_suite_info(session->suite);
            if (psk_info == nullptr || !psk_info->is_tls13) {
                return false;
            }
            in.has_psk = true;
            in.psk_identity = session->identity;
            in.psk_obfuscated_ticket_age = session->obfuscated_ticket_age;
            in.psk_binder_len = static_cast<std::uint8_t>(tls_hash_len(psk_info->hash));
            in.early_data = sent_early_ext && !second; // HRR kills 0-RTT (06 §2.2)
        }

        const auto encoded = tls_encode_client_hello(in, ch);
        if (!encoded.has_value()) {
            return false;
        }
        ch_len = encoded->len;
        binder_off = in.has_psk ? encoded->binder_block_offset : 0;
        return true;
    }

    // Computes and backfills the PSK binder over the truncated retained CH.
    // Requires `sched` freshly set_psk'd: binder_key is once-per-schedule, so
    // HRR rebuilds the schedule before calling this again.
    [[nodiscard]] bool backfill_binder() noexcept {
        if (binder_off == 0) {
            return true;
        }
        auto binder_key = sched->binder_key(TlsPskBinderKind::Resumption);
        if (!binder_key.has_value()) {
            return false;
        }
        const std::size_t mac_len = binder_key->len();
        TlsHash truncated;
        if (!truncated.init(sched->hash()) || !truncated.update({ch.data(), binder_off})) {
            return false;
        }
        std::array<std::uint8_t, 64> digest{};
        if (!truncated.final(digest)) {
            return false;
        }
        // Binder bytes start 3 past the binders-length prefix: be16(1+len),
        // u8(len), then the MAC.
        return tls13_psk_binder_mac(*binder_key, {digest.data(), mac_len}, {ch.data() + binder_off + 3, mac_len})
                .has_value();
    }

    [[nodiscard]] bool init_early_write() noexcept {
        TlsHash one_shot;
        if (!one_shot.init(sched->hash()) || !one_shot.update({ch.data(), ch_len})) {
            return false;
        }
        std::array<std::uint8_t, 64> digest{};
        if (!one_shot.final(digest)) {
            return false;
        }
        auto early_secret = sched->client_early_traffic_secret({digest.data(), tls_hash_len(sched->hash())});
        if (!early_secret.has_value()) {
            return false;
        }
        TlsRecordCipher fresh;
        auto derived = tls13_traffic_keys(*early_secret, session->suite);
        if (!derived.has_value()) {
            return false;
        }
        TlsTrafficKeys &keys = *derived;
        if (!fresh.init(session->suite, TlsRecordProtectionKind::Tls13, {keys.key.data(), keys.key_len},
                        {keys.iv.data(), keys.iv_len})
                     .has_value()) {
            return false;
        }
        tls_secure_wipe(keys.key.data(), keys.key.size());
        tls_secure_wipe(keys.iv.data(), keys.iv.size());
        early_write = std::move(fresh);
        return true;
    }

    // ---- ServerHello ----

    void handle_server_hello(std::span<const std::uint8_t> body) noexcept;
    void handle_hello_retry_request(const TlsServerHello &sh, std::span<const std::uint8_t> body) noexcept;
    void handle_server_hello_13(const TlsServerHello &sh, std::span<const std::uint8_t> body) noexcept;

    // ---- 1.3 server flight ----

    void handle_encrypted_extensions(std::span<const std::uint8_t> body) noexcept;
    void handle_certificate_request_13(std::span<const std::uint8_t> body) noexcept;
    void handle_certificate_13(std::span<const std::uint8_t> body) noexcept;
    void handle_certificate_verify_13(std::span<const std::uint8_t> body) noexcept;
    void handle_finished_13(std::span<const std::uint8_t> body) noexcept;
    [[nodiscard]] bool send_client_certificate_13() noexcept; // Cert + CV when credential present
    void finish_1_3() noexcept; // remaining client flight + Done

    // ---- 1.2 sub-flow (06 §4.3) ----

    void handle_server_hello_12(const TlsServerHello &sh, std::span<const std::uint8_t> body) noexcept;
    void handle_certificate_12(std::span<const std::uint8_t> body) noexcept;
    void handle_server_key_exchange_12(std::span<const std::uint8_t> body) noexcept;
    void handle_certificate_request_12(std::span<const std::uint8_t> body) noexcept;
    void handle_server_hello_done_12(std::span<const std::uint8_t> body) noexcept;
    [[nodiscard]] bool send_client_flight_12() noexcept; // [Cert [CV]] CKE CCS Fin (raises its own alerts)
    void handle_new_session_ticket_12(std::span<const std::uint8_t> body) noexcept;
    void handle_finished_12(std::span<const std::uint8_t> body) noexcept;
    void finish_1_2() noexcept; // Done

    void handle_message(TlsHandshakeType type, std::span<const std::uint8_t> body) noexcept;

    [[nodiscard]] common::IoResult<Event> pump() noexcept;
};

// =====================================================================
// ClientHello first flight
// =====================================================================

TlsClientHandshakeEngine::TlsClientHandshakeEngine(const TlsClientConfig &config, const TlsSessionOffer *session,
                                                   mem::IoBufNodePool &pool) noexcept {
    impl_ = new (std::nothrow) Impl(config, session);
    if (impl_ == nullptr) {
        return; // done()/failed() report the terminal state; no alert bytes
    }
    Impl &impl = *impl_;
    impl.ctx.bind(pool);
    impl.ctx.set_legacy_version(kTlsRecordVersionTls10); // first flight (06 §2.3)

    impl.psk_offered = session != nullptr;
    impl.sent_early_ext = impl.psk_offered && session->max_early_data > 0;

    if (impl.cfg.verify_peer && impl.cfg.trust == nullptr) {
        impl.fail_local(TlsAlertDesc::InternalError); // configuration bug, not a protocol event
        return;
    }

    // PSK offers need the schedule (and its once-only binder key) before the
    // CH bytes exist; the suite comes from the offer, not negotiation.
    if (impl.psk_offered) {
        const TlsSuiteInfo *psk_info = tls_suite_info(session->suite);
        if (psk_info == nullptr || !psk_info->is_tls13) {
            impl.fail_local(TlsAlertDesc::InternalError);
            return;
        }
        impl.sched.emplace(session->suite);
        if (!impl.sched->set_psk(session->psk).has_value()) {
            impl.fail_local(TlsAlertDesc::InternalError);
            return;
        }
    }

    if (!impl.build_client_hello(false, static_cast<std::uint16_t>(TlsNamedGroup::X25519), {}) ||
        (impl.psk_offered && !impl.backfill_binder()) || (impl.sent_early_ext && !impl.init_early_write()) ||
        !impl.emit_message({impl.ch.data(), impl.ch_len})) {
        impl.fail_local(TlsAlertDesc::InternalError);
        return;
    }
    if (impl.sent_early_ext) {
        // RFC 8446 D.4 / BoringSSL do_enter_early_data: when early data is
        // offered, the compat CCS belongs immediately after the first CH —
        // in front of the 0-RTT flight. From here on records are 0x0303
        // (the peer's record layer checks the version strictly once its read
        // cipher is live, even for the CCS it silently discards).
        impl.ctx.set_legacy_version(kTlsRecordVersionTls12);
        if (!impl.ctx.send_ccs().has_value()) {
            impl.fail_local(TlsAlertDesc::InternalError);
            return;
        }
        impl.ccs_sent = true;
    }
}

TlsClientHandshakeEngine::~TlsClientHandshakeEngine() { delete impl_; }

// =====================================================================
// feed / early data / output / state
// =====================================================================

common::IoResult<TlsClientHandshakeEngine::Event> TlsClientHandshakeEngine::feed(mem::IoBuf &&bytes) noexcept {
    FIBER_ASSERT(impl_ != nullptr && !impl_->done);
    if (!impl_->ctx.feed(std::move(bytes))) {
        return std::unexpected(common::IoErr::NoMem);
    }
    return impl_->pump();
}

common::IoResult<TlsClientHandshakeEngine::Event> TlsClientHandshakeEngine::feed(mem::IoBufChain &&bytes) noexcept {
    FIBER_ASSERT(impl_ != nullptr && !impl_->done);
    if (!impl_->ctx.feed(std::move(bytes))) {
        return std::unexpected(common::IoErr::NoMem);
    }
    return impl_->pump();
}

common::IoResult<void> TlsClientHandshakeEngine::write_early_data(std::span<const std::uint8_t> data) noexcept {
    if (impl_ == nullptr || !impl_->sent_early_ext || impl_->early_window_closed || impl_->done ||
        !impl_->early_write.initialized()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (impl_->early_written + data.size() > impl_->session->max_early_data) {
        return std::unexpected(common::IoErr::MessageTooLarge); // nothing written, earlier bytes kept
    }
    const auto emitted = impl_->ctx.emit(TlsContentType::ApplicationData, data, &impl_->early_write);
    if (!emitted.has_value()) {
        return std::unexpected(emitted.error());
    }
    impl_->early_written += data.size();
    return {};
}

mem::IoBufChain TlsClientHandshakeEngine::take_output() noexcept {
    if (impl_ == nullptr) {
        return mem::IoBufChain{};
    }
    return impl_->ctx.take_output();
}

bool TlsClientHandshakeEngine::done() const noexcept { return impl_ == nullptr || impl_->done; }

bool TlsClientHandshakeEngine::failed() const noexcept { return impl_ == nullptr || impl_->failed; }

TlsAlertDesc TlsClientHandshakeEngine::failure_alert() const noexcept {
    FIBER_ASSERT(done());
    return impl_ != nullptr ? impl_->failure_alert : TlsAlertDesc::InternalError;
}

TlsConnectedState TlsClientHandshakeEngine::take_state() noexcept {
    FIBER_ASSERT(impl_ != nullptr && impl_->done && !impl_->failed);
    return std::move(impl_->state);
}

// =====================================================================
// FSM pump
// =====================================================================

common::IoResult<TlsClientHandshakeEngine::Event> TlsClientHandshakeEngine::Impl::pump() noexcept {
    for (;;) {
        if (done) {
            return failed ? Event::Failed : Event::HandshakeDone;
        }
        const TlsInboundStep step = ctx.step();
        switch (step.kind) {
            case TlsInboundStep::Kind::NeedMore:
                return Event::None;
            case TlsInboundStep::Kind::Fatal:
                fail_local(step.alert);
                return Event::Failed;
            case TlsInboundStep::Kind::Alert:
                // Mid-handshake every inbound alert is terminal (06 §2.3) —
                // close_notify included: the peer walked away mid-flight. Nothing
                // is sent back to a connection that already told us it is dying.
                fail_peer(step.alert);
                return Event::Failed;
            case TlsInboundStep::Kind::Ccs:
                switch (st) {
                    case St::ExpectServerCcs12:
                        // The server's one CCS switches the read side to the key_block
                        // server keys; our write side went live with the client flight.
                        if (!swap_cipher_12(ctx.read_cipher(), kb12.server)) {
                            fail_local(TlsAlertDesc::InternalError);
                            return Event::Failed;
                        }
                        ctx.set_inbound_mode(TlsInboundMode::Sealed12);
                        st = St::ExpectServerFin12;
                        break;
                    case St::ExpectServerCert12:
                    case St::ExpectSke12:
                    case St::ExpectCrShd12:
                        // The server's CCS belongs after its flight and its tickets,
                        // right before its Finished — earlier than that is a fault.
                        fail_local(TlsAlertDesc::UnexpectedMessage);
                        return Event::Failed;
                    default:
                        // The 1.3 client ignores every well-formed peer CCS (middlebox
                        // compat, including the server's post-SH one); the context
                        // validated the 1-byte 0x01 form.
                        break;
                }
                break;
            case TlsInboundStep::Kind::Message:
                handle_message(step.type, step.body);
                break;
        }
    }
}

void TlsClientHandshakeEngine::Impl::handle_message(TlsHandshakeType type,
                                                    std::span<const std::uint8_t> body) noexcept {
    switch (st) {
        case St::WaitServerHello:
            if (type != TlsHandshakeType::ServerHello) {
                fail_local(TlsAlertDesc::UnexpectedMessage);
                return;
            }
            handle_server_hello(body);
            return;
        case St::ExpectEe:
            if (type != TlsHandshakeType::EncryptedExtensions) {
                fail_local(TlsAlertDesc::UnexpectedMessage);
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
                    fail_local(TlsAlertDesc::UnexpectedMessage);
                    return;
            }
        case St::ExpectCv:
            if (type != TlsHandshakeType::CertificateVerify) {
                fail_local(TlsAlertDesc::UnexpectedMessage);
                return;
            }
            handle_certificate_verify_13(body);
            return;
        case St::ExpectFin:
            if (type != TlsHandshakeType::Finished) {
                fail_local(TlsAlertDesc::UnexpectedMessage);
                return;
            }
            handle_finished_13(body);
            return;
        case St::ExpectServerCert12:
            if (type != TlsHandshakeType::Certificate) {
                fail_local(TlsAlertDesc::UnexpectedMessage);
                return;
            }
            handle_certificate_12(body);
            return;
        case St::ExpectSke12:
            if (type != TlsHandshakeType::ServerKeyExchange) {
                fail_local(TlsAlertDesc::UnexpectedMessage);
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
                    fail_local(TlsAlertDesc::UnexpectedMessage);
                    return;
            }
        case St::ExpectServerCcs12:
            // Handshake traffic between our flight and the server's CCS is
            // limited to session tickets; everything else waits for the cipher.
            if (type != TlsHandshakeType::NewSessionTicket) {
                fail_local(TlsAlertDesc::UnexpectedMessage);
                return;
            }
            handle_new_session_ticket_12(body);
            return;
        case St::ExpectServerFin12:
            if (type != TlsHandshakeType::Finished) {
                fail_local(TlsAlertDesc::UnexpectedMessage);
                return;
            }
            handle_finished_12(body);
            return;
        case St::Done:
            FIBER_ASSERT(false); // pump stops at done; a message here is an engine bug
            return;
    }
}

// =====================================================================
// ServerHello / HelloRetryRequest
// =====================================================================

void TlsClientHandshakeEngine::Impl::handle_server_hello(std::span<const std::uint8_t> body) noexcept {
    TlsServerHello sh;
    if (!tls_decode_server_hello(body.data(), body.size(), sh).has_value()) {
        fail_local(TlsAlertDesc::DecodeError);
        return;
    }

    // Version first (06 §4.2): supported_versions present → 1.3 semantics
    // (any other value is illegal_parameter); absent → the 1.2 sub-flow (the
    // CH offers [0x0304, 0x0303], so a conforming 1.2-only peer lands there).
    if (sh.has_supported_version && sh.supported_version != kTlsVersionTls13) {
        fail_local(TlsAlertDesc::IllegalParameter);
        return;
    }
    // 0x0303 for everything the client writes from here on — the peer's read
    // cipher is (or is about to be) live and it checks versions strictly,
    // including on alerts and the discarded compat CCS (06 §2.3). True for
    // 1.2 as well: every record after the initial CH flies at 0x0303.
    ctx.set_legacy_version(kTlsRecordVersionTls12);
    if (!sh.has_supported_version) {
        handle_server_hello_12(sh, body);
        return;
    }
    if (tls_is_hello_retry_request(sh.random)) {
        handle_hello_retry_request(sh, body);
        return;
    }
    handle_server_hello_13(sh, body);
}

void TlsClientHandshakeEngine::Impl::handle_hello_retry_request(const TlsServerHello &sh,
                                                                std::span<const std::uint8_t> body) noexcept {
    if (hrr_count >= 1) {
        fail_local(TlsAlertDesc::UnexpectedMessage); // a second HRR is a MUST-abort
        return;
    }
    if (sh.has_alpn || sh.has_extended_master_secret || sh.has_renegotiation_info) {
        // Extensions that cannot appear in an SH-family message.
        fail_local(TlsAlertDesc::UnsupportedExtension);
        return;
    }
    if (sh.compression_method != 0 || !sh.has_key_share || !sh.key_share.empty() || sh.has_selected_identity) {
        // Wrong-shaped HRR: missing selected_group key_share, a server share,
        // or a PSK selection that belongs in the real SH.
        fail_local(TlsAlertDesc::IllegalParameter);
        return;
    }
    const TlsSuiteInfo *info = tls_suite_info(static_cast<TlsCipherSuiteId>(sh.cipher_suite));
    if (suite_offer_index(sh.cipher_suite) == kOfferedSuites.size() || info == nullptr || !info->is_tls13) {
        fail_local(TlsAlertDesc::IllegalParameter);
        return;
    }
    if (sh.session_id.size() != session_id.size() ||
        std::memcmp(sh.session_id.data(), session_id.data(), session_id.size()) != 0) {
        fail_local(TlsAlertDesc::IllegalParameter);
        return;
    }
    const std::uint16_t selected = sh.key_share_group;
    if (!group_offered(selected) || selected == static_cast<std::uint16_t>(kx_group)) {
        // A group we never offered — or the one we already sent a share for
        // (the server should have used that share) — is a protocol fault.
        fail_local(TlsAlertDesc::IllegalParameter);
        return;
    }

    // Transcript restart (RFC 8446 §4.4.1): the synthetic message_hash wraps
    // ONLY Hash(CH1); the HRR itself is hashed as the first message of the
    // restarted transcript (BoringSSL hashes CH1, calls
    // UpdateForHelloRetryRequest, and add_message(HRR) lands after the
    // restart) — pinned by interop: Hash(msg_hash(Hash(CH1)) || HRR || CH2 || SH).
    if (!t13.init(info->hash) || !t13.update({ch.data(), ch_len}) || !t13.restart_message_hash()) {
        fail_local(TlsAlertDesc::InternalError);
        return;
    }
    feed13(TlsHandshakeType::ServerHello, body); // HRR is hashed as an SH

    // The 02 contract: schedule and key exchange are destroyed, not reused
    // (the restart invalidates their state); 0-RTT died with the HRR.
    kx.reset();
    sched.reset();
    early_write = TlsRecordCipher{};
    early_window_closed = true;
    sent_early_ext = false;

    if (!build_client_hello(true, selected, sh.has_cookie ? sh.cookie : std::span<const std::uint8_t>{})) {
        fail_local(TlsAlertDesc::InternalError);
        return;
    }
    if (psk_offered) {
        // PSK survives the HRR; the binder is recomputed over truncated CH2
        // with a fresh schedule (binder_key is once-per-instance).
        sched.emplace(session->suite);
        if (!sched->set_psk(session->psk).has_value() || !backfill_binder()) {
            fail_local(TlsAlertDesc::InternalError);
            return;
        }
    }
    // The one compat CCS flushes immediately before the second flight — here
    // CH2 — at legacy_record_version 0x0303 (BoringSSL queues it there; after
    // CH1 it only went out with early data, hence the once-flag).
    if ((!ccs_sent && !ctx.send_ccs().has_value()) || !t13.update({ch.data(), ch_len}) ||
        !emit_message({ch.data(), ch_len})) {
        fail_local(TlsAlertDesc::InternalError);
        return;
    }
    ccs_sent = true;
    hrr_count = 1;
    hrr_suite = static_cast<TlsCipherSuiteId>(sh.cipher_suite);
}

void TlsClientHandshakeEngine::Impl::handle_server_hello_13(const TlsServerHello &sh,
                                                            std::span<const std::uint8_t> body) noexcept {
    if (sh.compression_method != 0 || !sh.has_key_share || sh.key_share.empty() || sh.has_cookie) {
        // A real SH must carry the server's share (the empty selected_group
        // form belongs to HRR); cookie is HRR-only.
        fail_local(sh.has_cookie ? TlsAlertDesc::UnsupportedExtension : TlsAlertDesc::IllegalParameter);
        return;
    }
    if (sh.has_alpn || sh.has_extended_master_secret || sh.has_renegotiation_info) {
        fail_local(TlsAlertDesc::UnsupportedExtension); // 1.2-only echoes never appear in a 1.3 SH
        return;
    }
    if (suite_offer_index(sh.cipher_suite) == kOfferedSuites.size()) {
        fail_local(TlsAlertDesc::IllegalParameter);
        return;
    }
    suite = static_cast<TlsCipherSuiteId>(sh.cipher_suite);
    const TlsSuiteInfo *info = suite_info();
    if (info == nullptr || !info->is_tls13) {
        fail_local(TlsAlertDesc::IllegalParameter);
        return;
    }
    if (hrr_count == 1 && suite != hrr_suite) {
        fail_local(TlsAlertDesc::IllegalParameter); // transcript-hash stability (06 §2.1)
        return;
    }
    if (sh.session_id.size() != session_id.size() ||
        std::memcmp(sh.session_id.data(), session_id.data(), session_id.size()) != 0) {
        fail_local(TlsAlertDesc::IllegalParameter);
        return;
    }
    if (sh.key_share_group != static_cast<std::uint16_t>(kx_group)) {
        fail_local(TlsAlertDesc::IllegalParameter);
        return;
    }

    // PSK outcome (06 §2.5): accepted (index 0 of our single offer, suite
    // matching the binder's hash) or rejected (fresh no-PSK schedule). Either
    // way the early-data window closes at the SH read point.
    if (sh.has_selected_identity) {
        if (!psk_offered || sh.selected_identity != 0 || suite != session->suite) {
            fail_local(TlsAlertDesc::IllegalParameter);
            return;
        }
        psk_accepted = true;
    } else if (psk_offered) {
        // Rejected: the PSK-colored early tree is wrong — a fresh no-PSK
        // schedule over the negotiated suite.
        sched.reset();
    }
    if (!sched.has_value()) {
        // The no-PSK (or post-HRR) flow owns no schedule until the suite is
        // known — the SH read point is where it starts.
        sched.emplace(suite);
    }
    early_window_closed = true;

    const TlsKxShared z = kx->shared_secret(sh.key_share);
    if (z.status == TlsKxStatus::BadPeerData) {
        fail_local(TlsAlertDesc::IllegalParameter);
        return;
    }
    if (z.status != TlsKxStatus::Ok) {
        fail_local(TlsAlertDesc::InternalError);
        return;
    }

    // Transcript from the SH read point (06 §5.2): the retained CH bytes
    // first, then the SH. After an HRR the transcript already holds
    // message_hash + CH2 — only the SH is appended.
    if (hrr_count == 0) {
        if (!t13.init(info->hash) || !t13.update({ch.data(), ch_len})) {
            fail_local(TlsAlertDesc::InternalError);
            return;
        }
    }
    feed13(TlsHandshakeType::ServerHello, body);
    snapshot13(); // Hash(CH..SH)

    if (!sched->handshake_secrets({z.z.data(), z.z.size()}, {hash_buf.data(), hash_len()}, client_hs, server_hs)
                 .has_value()) {
        fail_local(TlsAlertDesc::InternalError);
        return;
    }
    {
        if (!swap_cipher(ctx.read_cipher(), server_hs)) {
            fail_local(TlsAlertDesc::InternalError);
            return;
        }
    }
    ctx.set_inbound_mode(TlsInboundMode::Sealed13);
    st = St::ExpectEe;
}

// =====================================================================
// 1.3 server flight
// =====================================================================

void TlsClientHandshakeEngine::Impl::handle_encrypted_extensions(std::span<const std::uint8_t> body) noexcept {
    TlsEncryptedExtensions ee;
    if (!tls_decode_encrypted_extensions(body.data(), body.size(), ee).has_value()) {
        fail_local(TlsAlertDesc::DecodeError);
        return;
    }
    // Extensions that only exist in the CH/SH/HRR must not reappear in EE
    // (RFC 8446 §4.2: the client MUST abort with unsupported_extension).
    TlsExtensionView view;
    for (TlsExtensionCursor walk(ee.extensions_block);;) {
        const auto entry = walk.next(view);
        if (!entry.has_value()) {
            fail_local(TlsAlertDesc::DecodeError);
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
                fail_local(TlsAlertDesc::UnsupportedExtension);
                return;
            default:
                break;
        }
    }

    if (ee.has_early_data && (!sent_early_ext || !psk_accepted)) {
        // The server accepted early data we never validly offered (a rejected
        // PSK kills any early_data offer along with it).
        fail_local(TlsAlertDesc::IllegalParameter);
        return;
    }
    feed13(TlsHandshakeType::EncryptedExtensions, body);

    if (ee.has_alpn) {
        bool offered = false;
        for (const std::string_view name: cfg.alpn) {
            if (name == ee.alpn) {
                offered = true;
                break;
            }
        }
        if (!offered || ee.alpn.size() > state.alpn.size()) {
            fail_local(TlsAlertDesc::IllegalParameter);
            return;
        }
        std::memcpy(state.alpn.data(), ee.alpn.data(), ee.alpn.size());
        state.alpn_len = static_cast<std::uint16_t>(ee.alpn.size());
    }
    ee_early_data = ee.has_early_data;
    st = St::ExpectCrCertFin;
}

void TlsClientHandshakeEngine::Impl::handle_certificate_request_13(std::span<const std::uint8_t> body) noexcept {
    if (cr13_received) {
        fail_local(TlsAlertDesc::UnexpectedMessage); // at most one CR, right after EE
        return;
    }
    TlsCertificateRequest13 cr;
    if (!tls_decode_certificate_request_13(body.data(), body.size(), cr).has_value()) {
        fail_local(TlsAlertDesc::DecodeError);
        return;
    }
    if (cr.signature_algorithms.size() % 2 != 0 || cr.signature_algorithms.size() / 2 > cr_sigalgs.size()) {
        fail_local(TlsAlertDesc::DecodeError);
        return;
    }
    feed13(TlsHandshakeType::CertificateRequest, body);
    cr13_received = true;
    cr_ctx_len = cr.certificate_request_context.size();
    if (cr_ctx_len > 0) {
        std::memcpy(cr_context.data(), cr.certificate_request_context.data(), cr_ctx_len);
    }
    cr_sigalgs_n = cr.signature_algorithms.size() / 2;
    for (std::size_t i = 0; i < cr_sigalgs_n; ++i) {
        cr_sigalgs[i] =
                static_cast<std::uint16_t>((cr.signature_algorithms[2 * i] << 8) | cr.signature_algorithms[2 * i + 1]);
    }
}

void TlsClientHandshakeEngine::Impl::handle_certificate_13(std::span<const std::uint8_t> body) noexcept {
    if (psk_accepted) {
        fail_local(TlsAlertDesc::UnexpectedMessage); // resumed sessions carry no Cert/CV
        return;
    }
    TlsCertificate13 cert;
    if (!tls_decode_certificate_13(body.data(), body.size(), cert).has_value() || cert.cert_count == 0) {
        fail_local(TlsAlertDesc::DecodeError); // a server chain is never empty
        return;
    }
    auto chain = TlsCertificateChain::from_der_list({cert.certs, cert.cert_count});
    if (!chain.has_value()) {
        fail_local(TlsAlertDesc::DecodeError);
        return;
    }
    peer_chain = std::move(chain).value();
    feed13(TlsHandshakeType::Certificate, body);

    if (cfg.verify_peer) {
        const auto verification = tls_verify_chain(peer_chain, *cfg.trust, TlsCertPurpose::SslServer, cfg.sni_host,
                                                   cfg.verify_ip, cfg.now_unix_ms);
        if (!verification.has_value()) {
            fail_local(TlsAlertDesc::InternalError);
            return;
        }
        if (verification->status == TlsCertVerification::Status::NotTrusted) {
            fail_local(verification->alert);
            return;
        }
    }
    st = St::ExpectCv;
}

void TlsClientHandshakeEngine::Impl::handle_certificate_verify_13(std::span<const std::uint8_t> body) noexcept {
    TlsCertificateVerify cv;
    if (!tls_decode_certificate_verify(body.data(), body.size(), cv).has_value()) {
        fail_local(TlsAlertDesc::DecodeError);
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
        fail_local(TlsAlertDesc::IllegalParameter);
        return;
    }
    const auto public_key = peer_chain.leaf().public_key();
    if (!public_key.has_value()) {
        fail_local(TlsAlertDesc::InternalError);
        return;
    }
    std::array<std::uint8_t, kCvContentMax> content{};
    const std::size_t content_len = 64 + kServerCvCtxLen + 1 + hash_len();
    build_cert_verify_content({content.data(), content_len}, {"TLS 1.3, server CertificateVerify", kServerCvCtxLen},
                              {hash_buf.data(), hash_len()});
    const auto valid = tls_verify(static_cast<TlsSignatureScheme>(cv.algorithm), TlsProtocolVersion::Tls13, *public_key,
                                  {content.data(), content_len}, cv.signature);
    if (!valid.has_value()) {
        fail_local(TlsAlertDesc::IllegalParameter); // scheme does not match the peer's key
        return;
    }
    if (!valid.value()) {
        fail_local(TlsAlertDesc::DecryptError);
        return;
    }
    feed13(TlsHandshakeType::CertificateVerify, body);
    st = St::ExpectFin;
}

void TlsClientHandshakeEngine::Impl::handle_finished_13(std::span<const std::uint8_t> body) noexcept {
    if (st == St::ExpectCrCertFin && !psk_accepted) {
        fail_local(TlsAlertDesc::UnexpectedMessage); // Fin without a certificate flight
        return;
    }
    TlsFinished fin;
    if (!tls_decode_finished(body.data(), body.size(), fin).has_value() || fin.verify_data.size() != hash_len()) {
        fail_local(TlsAlertDesc::DecodeError);
        return;
    }
    // verify_data covers the transcript through whatever preceded Fin —
    // snapshotted before Fin itself is fed.
    snapshot13();
    std::array<std::uint8_t, 48> expected{};
    if (!tls13_finished_mac(server_hs, {hash_buf.data(), hash_len()}, {expected.data(), hash_len()}).has_value()) {
        fail_local(TlsAlertDesc::InternalError);
        return;
    }
    if (!tls_constant_time_equal({expected.data(), hash_len()}, fin.verify_data)) {
        fail_local(TlsAlertDesc::DecryptError);
        return;
    }
    feed13(TlsHandshakeType::Finished, body);
    finish_1_3();
}

// Client Cert (+ CV when a credential is configured; an empty Cert when not).
// False = failure the caller raises (handshake_failure / internal_error).
bool TlsClientHandshakeEngine::Impl::send_client_certificate_13() noexcept {
    const bool have_credential = cfg.client_chain != nullptr && cfg.client_key != nullptr && !cfg.client_chain->empty();
    if (!have_credential) {
        // No credential: an empty chain — structurally valid, the server
        // decides whether that ends the handshake.
        const auto cert_len =
                tls_encode_certificate_13({cr_context.data(), cr_ctx_len}, {}, {scratch.data(), scratch.size()});
        if (!cert_len.has_value() || !t13.update({scratch.data(), cert_len.value()}) ||
            !emit_message({scratch.data(), cert_len.value()})) {
            return false;
        }
        return true;
    }

    const TlsCertificateChain &chain = *cfg.client_chain;
    std::array<std::span<const std::uint8_t>, TlsCertificateChain::kMaxCerts> ders{};
    ders[0] = chain.leaf().der();
    for (std::size_t i = 0; i < chain.intermediates().size(); ++i) {
        ders[i + 1] = chain.intermediates()[i].der();
    }
    const auto cert_len = tls_encode_certificate_13({cr_context.data(), cr_ctx_len}, {ders.data(), chain.size()},
                                                    {scratch.data(), scratch.size()});
    if (!cert_len.has_value() || !t13.update({scratch.data(), cert_len.value()}) ||
        !emit_message({scratch.data(), cert_len.value()})) {
        return false;
    }

    // CertificateVerify: first 1.3 preference the server offered in its CR
    // that the local key supports (02b selection loop).
    snapshot13(); // through our Cert
    std::array<std::uint8_t, kCvContentMax> content{};
    const std::size_t content_len = 64 + kClientCvCtxLen + 1 + hash_len();
    build_cert_verify_content({content.data(), content_len}, {"TLS 1.3, client CertificateVerify", kClientCvCtxLen},
                              {hash_buf.data(), hash_len()});
    std::optional<TlsSignatureScheme> chosen;
    for (const TlsSignatureScheme scheme: kTls13SignaturePreference) {
        if (!cfg.client_key->supports(scheme, TlsProtocolVersion::Tls13)) {
            continue;
        }
        for (std::size_t i = 0; i < cr_sigalgs_n; ++i) {
            if (cr_sigalgs[i] == static_cast<std::uint16_t>(scheme)) {
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
    std::array<std::uint8_t, kMaxSigLen> sig{};
    FIBER_ASSERT(cfg.client_key->max_signature_len() <= sig.size());
    const auto sig_len = cfg.client_key->sign(*chosen, TlsProtocolVersion::Tls13, {content.data(), content_len}, sig);
    if (!sig_len.has_value()) {
        return false;
    }
    const auto cv_len = tls_encode_certificate_verify(static_cast<std::uint16_t>(*chosen),
                                                      {sig.data(), sig_len.value()}, {scratch.data(), scratch.size()});
    if (!cv_len.has_value() || !t13.update({scratch.data(), cv_len.value()}) ||
        !emit_message({scratch.data(), cv_len.value()})) {
        return false;
    }
    return true;
}

void TlsClientHandshakeEngine::Impl::finish_1_3() noexcept {
    // Application secrets over Hash(CH..server Fin) — the transcript already
    // includes the verified Fin. The read side swaps to a fresh server_app0
    // instance now; the write side rides client_hs for the flight below and
    // swaps last (06 §5.3).
    snapshot13();
    if (!sched->application_secrets({hash_buf.data(), hash_len()}, client_app0, server_app0).has_value() ||
        !swap_cipher(ctx.read_cipher(), server_app0) || !swap_cipher(ctx.write_cipher(), client_hs)) {
        fail_local(TlsAlertDesc::InternalError);
        return;
    }

    // ---- client flight (sealed with the client_hs cipher) ----
    // Plain-path compat CCS: immediately before the second flight (RFC 8446
    // D.4), plaintext 0x0303 — the peer's record layer discards it before
    // routing; skipped when the early-data or HRR path already sent it.
    if (!ccs_sent && !ctx.send_ccs().has_value()) {
        fail_local(TlsAlertDesc::InternalError);
        return;
    }
    ccs_sent = true;
    if (cr13_received && !send_client_certificate_13()) {
        fail_local(TlsAlertDesc::HandshakeFailure);
        return;
    }
    if (sent_early_ext && psk_accepted && ee_early_data) {
        // EndOfEarlyData closes the accepted 0-RTT window.
        const auto eoed =
                tls_encode_handshake_message(TlsHandshakeType::EndOfEarlyData, {}, {scratch.data(), scratch.size()});
        if (!eoed.has_value() || !t13.update({scratch.data(), eoed.value()}) ||
            !emit_message({scratch.data(), eoed.value()})) {
            fail_local(TlsAlertDesc::InternalError);
            return;
        }
    }
    early_window_closed = true;

    // client Finished — MAC over the transcript before itself.
    snapshot13();
    std::array<std::uint8_t, 48> verify_data{};
    if (!tls13_finished_mac(client_hs, {hash_buf.data(), hash_len()}, {verify_data.data(), hash_len()}).has_value()) {
        fail_local(TlsAlertDesc::InternalError);
        return;
    }
    const auto fin_len = tls_encode_finished({verify_data.data(), hash_len()}, {scratch.data(), scratch.size()});
    if (!fin_len.has_value() || !t13.update({scratch.data(), fin_len.value()}) ||
        !emit_message({scratch.data(), fin_len.value()})) {
        fail_local(TlsAlertDesc::InternalError);
        return;
    }

    // Resumption base over Hash(CH..client Fin) — after feeding it — then the
    // write side swaps to client_app0; the hs instance dies with the flight.
    snapshot13();
    auto resumption = sched->resumption_master_secret({hash_buf.data(), hash_len()});
    if (!resumption.has_value() || !swap_cipher(ctx.write_cipher(), client_app0)) {
        fail_local(TlsAlertDesc::InternalError);
        return;
    }
    resumption_master = std::move(resumption).value();

    // ---- Done ----
    state.version = TlsProtocolVersion::Tls13;
    state.suite = suite;
    state.read_cipher = std::move(ctx.read_cipher());
    state.write_cipher = std::move(ctx.write_cipher());
    state.client_app_secret = std::move(client_app0);
    state.server_app_secret = std::move(server_app0);
    state.resumption_master = std::move(resumption_master);
    state.session_resumed = psk_accepted;
    state.early_data_accepted = sent_early_ext && psk_accepted && ee_early_data;
    state.peer_chain = std::move(peer_chain);
    done = true;
    st = St::Done;
    // Reader bytes arriving after the server Fin (e.g. an NST in the same
    // feed) stay buffered — post-handshake messages belong to 08/09.
}

// =====================================================================
// TLS 1.2 sub-flow (06 §4.3)
// =====================================================================

void TlsClientHandshakeEngine::Impl::handle_server_hello_12(const TlsServerHello &sh,
                                                            std::span<const std::uint8_t> body) noexcept {
    // The CH offered [0x0304, 0x0303]; anything but 0x0303 here is a version
    // we never offered (and any 0-RTT window died with the 1.2 negotiation).
    if (sh.legacy_version != kTlsVersionTls12) {
        fail_local(TlsAlertDesc::ProtocolVersion);
        return;
    }
    // RFC 8446 §4.1.3 downgrade sentinel: a 1.3-capable server forced down to
    // 1.2 flags itself with DOWNGRD||0x01 in the last 8 random bytes — the
    // downgrade was injected, the handshake is dead.
    static constexpr std::array<std::uint8_t, 8> kDowngradeSentinel{0x44, 0x4F, 0x57, 0x4E, 0x47, 0x52, 0x44, 0x01};
    if (std::memcmp(sh.random.data() + 24, kDowngradeSentinel.data(), kDowngradeSentinel.size()) == 0) {
        fail_local(TlsAlertDesc::IllegalParameter);
        return;
    }
    if (sh.compression_method != 0) {
        fail_local(TlsAlertDesc::IllegalParameter);
        return;
    }
    if (sh.has_key_share || sh.has_cookie) {
        fail_local(TlsAlertDesc::UnsupportedExtension); // 1.3-only extensions
        return;
    }
    if (sh.has_selected_identity) {
        fail_local(TlsAlertDesc::IllegalParameter); // PSK selection is 1.3-only
        return;
    }
    // RFC 5746 §3.4: we offered the empty initial RI; the echo must be present
    // and empty — the payload is exactly the 1-byte length prefix 0x00 of an
    // empty renegotiated_connection. Missing or renegotiation-bearing
    // (longer) both end the handshake.
    if (!sh.has_renegotiation_info || sh.renegotiation_info.size() != 1 || sh.renegotiation_info[0] != 0) {
        fail_local(TlsAlertDesc::HandshakeFailure);
        return;
    }
    if (suite_offer_index(sh.cipher_suite) == kOfferedSuites.size()) {
        fail_local(TlsAlertDesc::IllegalParameter); // never offered
        return;
    }
    suite = static_cast<TlsCipherSuiteId>(sh.cipher_suite);
    const TlsSuiteInfo *info = suite_info();
    if (info == nullptr || info->is_tls13) {
        fail_local(TlsAlertDesc::IllegalParameter); // a 1.3 suite in a 1.2 SH
        return;
    }
    if (sh.has_alpn) {
        bool offered = false;
        for (const std::string_view name: cfg.alpn) {
            if (name == sh.alpn) {
                offered = true;
                break;
            }
        }
        if (!offered || sh.alpn.size() > state.alpn.size()) {
            fail_local(TlsAlertDesc::IllegalParameter); // RFC 7301 §3.1
            return;
        }
        std::memcpy(state.alpn.data(), sh.alpn.data(), sh.alpn.size());
        state.alpn_len = static_cast<std::uint16_t>(sh.alpn.size());
    }

    std::memcpy(server_random.data(), sh.random.data(), server_random.size());
    ems_negotiated = sh.has_extended_master_secret; // we always offer EMS
    early_window_closed = true; // any PSK/early-data offer is dead in 1.2
    // Server records stay plaintext until its CCS; the session_id echo is the
    // server's own choice in 1.2 (it names ITS session, not ours).
    ctx.set_inbound_mode(TlsInboundMode::Plaintext12);

    if (!t12.init(info->hash) || !t12.update({ch.data(), ch_len})) {
        fail_local(TlsAlertDesc::InternalError);
        return;
    }
    feed12(TlsHandshakeType::ServerHello, body);
    st = St::ExpectServerCert12;
}

void TlsClientHandshakeEngine::Impl::handle_certificate_12(std::span<const std::uint8_t> body) noexcept {
    TlsCertificate12 cert;
    if (!tls_decode_certificate_12(body.data(), body.size(), cert).has_value() || cert.cert_count == 0) {
        fail_local(TlsAlertDesc::DecodeError); // a server chain is never empty
        return;
    }
    auto chain = TlsCertificateChain::from_der_list({cert.certs, cert.cert_count});
    if (!chain.has_value()) {
        fail_local(TlsAlertDesc::DecodeError);
        return;
    }
    peer_chain = std::move(chain).value();
    feed12(TlsHandshakeType::Certificate, body);

    if (cfg.verify_peer) {
        const auto verification = tls_verify_chain(peer_chain, *cfg.trust, TlsCertPurpose::SslServer, cfg.sni_host,
                                                   cfg.verify_ip, cfg.now_unix_ms);
        if (!verification.has_value()) {
            fail_local(TlsAlertDesc::InternalError);
            return;
        }
        if (verification->status == TlsCertVerification::Status::NotTrusted) {
            fail_local(verification->alert);
            return;
        }
    }
    st = St::ExpectSke12;
}

void TlsClientHandshakeEngine::Impl::handle_server_key_exchange_12(std::span<const std::uint8_t> body) noexcept {
    TlsServerKeyExchange ske;
    if (!tls_decode_server_key_exchange(body.data(), body.size(), ske).has_value()) {
        fail_local(TlsAlertDesc::IllegalParameter); // e.g. non-named_curve forms
        return;
    }
    if (!group_offered(ske.named_group) || !sigalg_offered(ske.algorithm)) {
        fail_local(TlsAlertDesc::IllegalParameter);
        return;
    }

    // The signature covers the 1.2 "ServerDHParams" prefixed with both
    // randoms — client_random || server_random || curve_type(3) ||
    // be16(group) || u8(point_len) || point (BoringSSL handshake_server.cc
    // builds and signs exactly these bytes; it is NOT a transcript digest).
    static constexpr std::size_t kSkeContentMax = 64 + 1 + 2 + 1 + 255; // randoms + params + u8 point cap
    std::array<std::uint8_t, kSkeContentMax> content{};
    std::memcpy(content.data(), client_random.data(), client_random.size());
    std::memcpy(content.data() + 32, server_random.data(), server_random.size());
    std::size_t off = 64;
    content[off++] = 0x03; // curve_type named_curve
    content[off++] = static_cast<std::uint8_t>(ske.named_group >> 8);
    content[off++] = static_cast<std::uint8_t>(ske.named_group);
    content[off++] = static_cast<std::uint8_t>(ske.public_key.size());
    std::memcpy(content.data() + off, ske.public_key.data(), ske.public_key.size());
    off += ske.public_key.size();

    const auto public_key = peer_chain.leaf().public_key();
    if (!public_key.has_value()) {
        fail_local(TlsAlertDesc::InternalError);
        return;
    }
    const auto valid = tls_verify(static_cast<TlsSignatureScheme>(ske.algorithm), TlsProtocolVersion::Tls12,
                                  *public_key, {content.data(), off}, ske.signature);
    if (!valid.has_value()) {
        fail_local(TlsAlertDesc::IllegalParameter); // scheme does not match the peer's key
        return;
    }
    if (!valid.value()) {
        fail_local(TlsAlertDesc::DecryptError);
        return;
    }

    // The server's curve wins: rebuild the ephemeral pair when it differs
    // from the group of the CH share (the 1.2 server ignores key_share — the
    // curve arrives here, in the SKE).
    const auto group = static_cast<TlsNamedGroup>(ske.named_group);
    if (group != kx_group) {
        kx.reset();
        kx.emplace(group);
        if (!kx->generate().has_value()) {
            fail_local(TlsAlertDesc::InternalError);
            return;
        }
        kx_group = group;
    }
    const TlsKxShared z = kx->shared_secret(ske.public_key);
    if (z.status == TlsKxStatus::BadPeerData) {
        fail_local(TlsAlertDesc::IllegalParameter);
        return;
    }
    if (z.status != TlsKxStatus::Ok) {
        fail_local(TlsAlertDesc::InternalError);
        return;
    }
    z12 = z;

    feed12(TlsHandshakeType::ServerKeyExchange, body);
    st = St::ExpectCrShd12;
}

void TlsClientHandshakeEngine::Impl::handle_certificate_request_12(std::span<const std::uint8_t> body) noexcept {
    if (cr12_received) {
        fail_local(TlsAlertDesc::UnexpectedMessage); // at most one CR per flight
        return;
    }
    TlsCertificateRequest12 cr;
    if (!tls_decode_certificate_request_12(body.data(), body.size(), cr).has_value()) {
        fail_local(TlsAlertDesc::DecodeError);
        return;
    }
    if (cr.signature_algorithms.size() % 2 != 0 || cr.signature_algorithms.size() / 2 > cr_sigalgs.size()) {
        fail_local(TlsAlertDesc::DecodeError);
        return;
    }
    feed12(TlsHandshakeType::CertificateRequest, body);
    cr12_received = true;
    cr_sigalgs_n = cr.signature_algorithms.size() / 2; // 0 = absent = no constraint
    for (std::size_t i = 0; i < cr_sigalgs_n; ++i) {
        cr_sigalgs[i] =
                static_cast<std::uint16_t>((cr.signature_algorithms[2 * i] << 8) | cr.signature_algorithms[2 * i + 1]);
    }
    // stays in ExpectCrShd12 — ServerHelloDone still terminates the flight
}

void TlsClientHandshakeEngine::Impl::handle_server_hello_done_12(std::span<const std::uint8_t> body) noexcept {
    if (!body.empty()) {
        fail_local(TlsAlertDesc::DecodeError); // SHD carries no payload
        return;
    }
    feed12(TlsHandshakeType::ServerHelloDone, body);
    if (!send_client_flight_12()) {
        return; // the flight raised its own alert
    }
    st = St::ExpectServerCcs12;
}

// [Cert [CV]] CKE CCS Fin — every failure path raises its own alert. The
// client Cert/CKE/CV fly plaintext; the CCS swaps the write cipher, so the
// client Fin is the first sealed record.
bool TlsClientHandshakeEngine::Impl::send_client_flight_12() noexcept {
    // ---- client Certificate (only under a CertificateRequest) ----
    if (cr12_received) {
        const bool have_credential =
                cfg.client_chain != nullptr && cfg.client_key != nullptr && !cfg.client_chain->empty();
        std::size_t cert_len = 0;
        if (have_credential) {
            // 1.2 chain form: leaf + intermediates, no request context.
            const TlsCertificateChain &chain = *cfg.client_chain;
            std::array<std::span<const std::uint8_t>, TlsCertificateChain::kMaxCerts> ders{};
            ders[0] = chain.leaf().der();
            for (std::size_t i = 0; i < chain.intermediates().size(); ++i) {
                ders[i + 1] = chain.intermediates()[i].der();
            }
            const auto encoded =
                    tls_encode_certificate_12({ders.data(), chain.size()}, {scratch.data(), scratch.size()});
            if (!encoded.has_value()) {
                fail_local(TlsAlertDesc::InternalError);
                return false;
            }
            cert_len = encoded.value();
            sent_cert12 = true;
        } else {
            // No credential: an empty chain — structurally valid; the server
            // that asked decides whether that ends the handshake.
            const auto encoded = tls_encode_certificate_12({}, {scratch.data(), scratch.size()});
            if (!encoded.has_value()) {
                fail_local(TlsAlertDesc::InternalError);
                return false;
            }
            cert_len = encoded.value();
        }
        if (!t12.update({scratch.data(), cert_len}) || !emit_message({scratch.data(), cert_len})) {
            fail_local(TlsAlertDesc::InternalError);
            return false;
        }
    }

    // ---- ClientKeyExchange (plaintext): u8(point_len) || point ----
    const auto cke_len = tls_encode_client_key_exchange(kx->public_value().bytes(), {scratch.data(), scratch.size()});
    if (!cke_len.has_value() || !t12.update({scratch.data(), cke_len.value()}) ||
        !emit_message({scratch.data(), cke_len.value()})) {
        fail_local(TlsAlertDesc::InternalError);
        return false;
    }

    // ---- master secret + key_block ----
    // The EMS session_hash is the LIVE transcript at derivation time
    // (BoringSSL ssl_hash_message(CKE) / ssl_add_message_cbb(CKE) both run
    // BEFORE tls1_generate_master_secret): CH..SHD, our own Certificate, AND
    // the CKE — only the still-unwritten CV is excluded.
    if (ems_negotiated) {
        snapshot12();
        auto master = tls12_extended_master_secret(suite, z12.z, {hash_buf.data(), hash_len()});
        if (!master.has_value()) {
            fail_local(TlsAlertDesc::InternalError);
            return false;
        }
        master12 = std::move(master).value();
    } else {
        auto master = tls12_master_secret(suite, z12.z, client_random, server_random);
        if (!master.has_value()) {
            fail_local(TlsAlertDesc::InternalError);
            return false;
        }
        master12 = std::move(master).value();
    }
    auto keys = tls12_key_block(suite, master12, client_random, server_random);
    if (!keys.has_value()) {
        fail_local(TlsAlertDesc::InternalError);
        return false;
    }
    kb12 = keys.value();

    // ---- CertificateVerify over the RAW transcript bytes ----
    // A 1.2 CV signs the concatenated handshake_messages themselves
    // (BoringSSL signs hs->transcript.buffer(); EVP digests per scheme
    // internally — no pre-digested form exists). Sent only when our
    // Certificate was non-empty.
    if (sent_cert12) {
        std::optional<TlsSignatureScheme> chosen;
        for (const TlsSignatureScheme scheme: kTls12SignaturePreference) {
            if (!cfg.client_key->supports(scheme, TlsProtocolVersion::Tls12)) {
                continue;
            }
            bool offered_by_cr = cr_sigalgs_n == 0; // absent list = no constraint
            for (std::size_t i = 0; !offered_by_cr && i < cr_sigalgs_n; ++i) {
                offered_by_cr = cr_sigalgs[i] == static_cast<std::uint16_t>(scheme);
            }
            if (offered_by_cr) {
                chosen = scheme;
                break;
            }
        }
        if (!chosen.has_value()) {
            fail_local(TlsAlertDesc::HandshakeFailure); // no common scheme
            return false;
        }
        std::array<std::uint8_t, kMaxSigLen> sig{};
        FIBER_ASSERT(cfg.client_key->max_signature_len() <= sig.size());
        const auto sig_len = cfg.client_key->sign(*chosen, TlsProtocolVersion::Tls12, t12.buffer(), sig);
        if (!sig_len.has_value()) {
            fail_local(TlsAlertDesc::InternalError);
            return false;
        }
        const auto cv_len = tls_encode_certificate_verify(
                static_cast<std::uint16_t>(*chosen), {sig.data(), sig_len.value()}, {scratch.data(), scratch.size()});
        if (!cv_len.has_value() || !t12.update({scratch.data(), cv_len.value()}) ||
            !emit_message({scratch.data(), cv_len.value()})) {
            fail_local(TlsAlertDesc::InternalError);
            return false;
        }
    }

    // ---- CCS + write-cipher swap: everything after this is sealed ----
    if (!ctx.send_ccs().has_value() || !swap_cipher_12(ctx.write_cipher(), kb12.client)) {
        fail_local(TlsAlertDesc::InternalError);
        return false;
    }

    // ---- client Finished (sealed; MAC over the pre-Fin transcript) ----
    snapshot12();
    auto verify = tls12_verify_data(suite, master12, true, {hash_buf.data(), hash_len()});
    if (!verify.has_value()) {
        fail_local(TlsAlertDesc::InternalError);
        return false;
    }
    const auto fin_len = tls_encode_finished({verify->data(), verify->size()}, {scratch.data(), scratch.size()});
    if (!fin_len.has_value() || !t12.update({scratch.data(), fin_len.value()}) ||
        !emit_message({scratch.data(), fin_len.value()})) {
        fail_local(TlsAlertDesc::InternalError);
        return false;
    }
    return true;
}

void TlsClientHandshakeEngine::Impl::handle_new_session_ticket_12(std::span<const std::uint8_t> body) noexcept {
    // RFC 5077 §3.3: lifetime(4) + ticket_len(2) + ticket, no extension block
    // (that is a 1.3 NST). Tickets belong to 08's cache — the handshake merely
    // tolerates (and transcript-hashes) any number before the server's CCS.
    if (body.size() < 6 || (static_cast<std::size_t>(body[4]) << 8 | body[5]) + 6 != body.size()) {
        fail_local(TlsAlertDesc::DecodeError);
        return;
    }
    feed12(TlsHandshakeType::NewSessionTicket, body);
}

void TlsClientHandshakeEngine::Impl::handle_finished_12(std::span<const std::uint8_t> body) noexcept {
    TlsFinished fin;
    if (!tls_decode_finished(body.data(), body.size(), fin).has_value() || fin.verify_data.size() != 12) {
        fail_local(TlsAlertDesc::DecodeError); // 1.2 verify_data is always 12 bytes
        return;
    }
    // verify_data MACs the message list through OUR flight (the CCS is not a
    // handshake message) — snapshotted before the server Fin itself is fed.
    snapshot12();
    const auto expected = tls12_verify_data(suite, master12, false, {hash_buf.data(), hash_len()});
    if (!expected.has_value()) {
        fail_local(TlsAlertDesc::InternalError);
        return;
    }
    if (!tls_constant_time_equal({expected->data(), expected->size()}, fin.verify_data)) {
        fail_local(TlsAlertDesc::DecryptError);
        return;
    }
    feed12(TlsHandshakeType::Finished, body);
    finish_1_2();
}

void TlsClientHandshakeEngine::Impl::finish_1_2() noexcept {
    state.version = TlsProtocolVersion::Tls12;
    state.suite = suite;
    state.read_cipher = std::move(ctx.read_cipher());
    state.write_cipher = std::move(ctx.write_cipher());
    state.tls12_master = std::move(master12);
    state.session_resumed = false;
    state.early_data_accepted = false;
    state.peer_chain = std::move(peer_chain);
    wipe_kb12(); // both ciphers hold the key_block material now
    done = true;
    st = St::Done;
    // Reader bytes after the server Fin stay buffered — post-handshake
    // messages (tickets under a renegotiated-connection banner) belong to 08.
}

} // namespace fiber::tls
