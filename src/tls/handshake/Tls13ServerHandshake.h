#ifndef FIBER_TLS_HANDSHAKE_TLS13_SERVER_HANDSHAKE_H
#define FIBER_TLS_HANDSHAKE_TLS13_SERVER_HANDSHAKE_H

// TLS 1.3 server sub-flow (07 §4.2) — internal to the server engine. The
// outer TlsServerHandshakeEngine retains and decodes the ClientHello, makes
// the version call at the fork, and mounts this sub-flow; from there this
// class owns everything 1.3-specific: parameter selection (suite × group ×
// the client's key_share), the ServerHello flight (SH/EE/Cert/CV/Fin), the
// client-flight verification, and the finish that fills TlsConnectedState.
//
// Mounted by value into the outer's std::variant — never moved, never
// copied. Everything long-lived is BORROWED through Mount (the outer and its
// buffers outlive the sub); only flow state is owned here. Failures go
// through fail(), which encodes the fatal alert into the context and writes
// the shared outcome — no return-value plumbing.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

#include "../crypto/TlsCryptoPrimitives.h"
#include "../detail/TlsHandshakeContext.h"
#include "TlsServerHandshakeShared.h"
#include "TlsTranscript.h"

#include <fiber/tls/TlsConnectedState.h>
#include <fiber/tls/TlsTypes.h>
#include <fiber/tls/crypto/Tls13KeySchedule.h>
#include <fiber/tls/crypto/TlsCertificate.h>
#include <fiber/tls/handshake/TlsHandshakeCodec.h>
#include <fiber/tls/handshake/TlsHandshakeMessage.h>

namespace fiber::tls {

class Tls13ServerHandshake final : public common::NonCopyable, common::NonMovable {
public:
    // Borrowed from the outer engine — every member outlives the sub.
    struct Mount {
        TlsHandshakeContext &ctx; // shared record pipeline
        const TlsServerConfig &cfg;
        const TlsResumptionLookup *resumption; // nullptr = never resume (full handshake)
        const TlsTicketMinter *minter; // nullptr = never mint an NST
        TlsServerHelloState &hello; // retained CH + negotiation intermediates
        mem::IoBufChain &early; // decrypted 0-RTT plaintext (P5; the take_early_data backing)
        TlsServerHandshakeOutcome &out; // terminal channel
        std::span<std::uint8_t> scratch; // server-flight staging (outer array)
    };

    explicit Tls13ServerHandshake(const Mount &mount) noexcept;
    ~Tls13ServerHandshake(); // wipes traffic/resumption secrets

    // Entry point: the first inbound ClientHello. `ch` is the decoded view
    // (borrowing hello.ch — stable) and `body` the raw message body over the
    // same retained storage (no 4-byte header; feed13 reconstructs it).
    void start(const TlsClientHello &ch, std::span<const std::uint8_t> body) noexcept;

    // Post-fork inbound traffic.
    void on_message(TlsHandshakeType type, std::span<const std::uint8_t> body) noexcept;
    void on_ccs() noexcept {
        // The 1.3 server ignores every well-formed client CCS (middlebox
        // compat, including a pre-flight one); the context already validated
        // the 1-byte 0x01 form.
    }

    // Requires done && !failed (the outer's take_state asserts it).
    [[nodiscard]] TlsConnectedState take_state() noexcept { return std::move(state_); }

    // QUIC mode twin of take_state (the outer's take_quic_result asserts
    // done && !failed; only this mode fills it).
    [[nodiscard]] TlsQuicHandshakeResult take_quic_result() noexcept { return std::move(quic_result_); }

private:
    enum class St : std::uint8_t {
        WaitClientHello2, // an HRR is out; CH2 owed (exactly one HRR per handshake)
        WaitClientCert, // mTLS: the client flight opens with its Certificate
        WaitClientCv, // mTLS: CertificateVerify over the transcript through Cert
        WaitEndOfEarlyData, // 0-RTT accepted: inner app data sinks until EOED
        WaitClientFin, // the flight always ends with the client Finished
        Done,
    };

    // ---- borrowed (Mount) ----
    TlsHandshakeContext &ctx_;
    const TlsServerConfig &cfg_;
    const TlsResumptionLookup *resumption_;
    const TlsTicketMinter *minter_;
    TlsServerHelloState &hello_;
    mem::IoBufChain &early_;
    TlsServerHandshakeOutcome &out_;
    std::span<std::uint8_t> scratch_;

    // ---- owned flow state ----
    St st_ = St::WaitClientFin;
    TlsTranscript13 t13_;
    std::array<std::uint8_t, 64> hash_buf_{};
    std::array<std::uint8_t, 64> ch_hash_{}; // Hash(ClientHello) — client_early_traffic_secret input
    std::optional<TlsKeySchedule13> sched_;
    TlsSecret client_hs_{};
    TlsSecret server_hs_{};
    TlsSecret client_app0_{};
    TlsSecret server_app0_{};
    TlsSecret resumption_master_{};
    TlsSecret client_early_{}; // 0-RTT read keys' secret (derived Early-stage in start)
    TlsSecret resumed_psk_{}; // the accepted ticket's PSK (copied; CH2 binder re-verify)
    TlsCipherSuiteId suite_ = TlsCipherSuiteId::TlsAes128GcmSha256;
    TlsNamedGroup hrr_group_ = TlsNamedGroup::X25519; // the group the HRR requested
    // ---- resumption / 0-RTT (P5; §2.4 cascade outcome) ----
    std::string_view resumed_alpn_{}; // the ticket's early_alpn (borrowed from the lookup caller)
    std::uint32_t resumed_age_add_ = 0;
    std::uint32_t resumed_max_early_ = 0;
    bool psk_accepted_ = false;
    bool psk_dhe_ke_offered_ = false; // CH offered mode 1 (NST mint precondition)
    bool early_accepted_ = false;
    bool ccs_sent_ = false; // the one compat CCS went out (after HRR or SH; RFC 8446 D.4)
    TlsCertificateChain peer_chain_; // the mTLS client chain (empty otherwise)
    TlsConnectedState state_{};
    TlsQuicHandshakeResult quic_result_{}; // filled instead of state_ in QUIC mode (10 §8)

    // ---- helpers ----

    [[nodiscard]] const TlsSuiteInfo *suite_info() const noexcept { return tls_suite_info(suite_); }

    [[nodiscard]] std::size_t hash_len() const noexcept { return tls_hash_len(suite_info()->hash); }

    void fail(TlsAlertDesc alert) noexcept {
        if (out_.done) {
            return;
        }
        ctx_.fail(alert); // encodes the fatal alert (sealed when the write cipher is live)
        out_.failed = true;
        out_.done = true;
        out_.alert = alert;
        st_ = St::Done;
    }

    // Feeds a ctx-borrowed message BODY (no header) into the transcript —
    // the header is reconstructed from (type, body.size()).
    void feed13(TlsHandshakeType type, std::span<const std::uint8_t> body) noexcept {
        const std::array<std::uint8_t, 4> header{static_cast<std::uint8_t>(type), 0,
                                                 static_cast<std::uint8_t>(body.size() >> 8),
                                                 static_cast<std::uint8_t>(body.size())};
        (void) t13_.update(header);
        (void) t13_.update(body);
    }

    void snapshot13() noexcept { (void) t13_.snapshot_digest({hash_buf_.data(), hash_len()}); }

    // Emits a fully-encoded handshake message (header+body in `msg`); the
    // context seals it when the write cipher is live. False = connection-level
    // failure already encoded.
    [[nodiscard]] bool emit_message(std::span<const std::uint8_t> msg) noexcept {
        return ctx_.emit(TlsContentType::Handshake, msg).has_value();
    }

    // Derives traffic keys from `secret` and swaps a FRESH cipher instance
    // into `slot` (06's swap_cipher form: re-init of a live instance is a 05
    // contract violation; every key change is an instance swap).
    [[nodiscard]] bool swap_cipher(TlsRecordCipher &slot, const TlsSecret &secret) noexcept {
        TlsRecordCipher fresh;
        auto derived = tls13_traffic_keys(secret, suite_);
        if (!derived.has_value()) {
            return false;
        }
        TlsTrafficKeys &keys = *derived;
        if (!fresh.init(suite_, TlsRecordProtectionKind::Tls13, {keys.key.data(), keys.key_len},
                        {keys.iv.data(), keys.iv_len})
                     .has_value()) {
            return false;
        }
        tls_secure_wipe(keys.key.data(), keys.key.size());
        tls_secure_wipe(keys.iv.data(), keys.iv.size());
        slot = std::move(fresh);
        return true;
    }

    // QUIC secret export (10 §4): the callback twin of swap_cipher at each
    // derivation point — no record cipher is ever built.
    [[nodiscard]] bool quic_secret(TlsQuicLevel level, bool write_secret, const TlsSecret &secret) noexcept {
        return cfg_.quic->set_secret(cfg_.quic->ctx, level, write_secret, suite_, secret.bytes());
    }

    // Negotiated ALPN lands in whichever result the mode hands over (the
    // flight builder is the single fill point).
    void record_alpn(std::string_view alpn) noexcept {
        std::array<std::uint8_t, 256> &bytes = cfg_.quic != nullptr ? quic_result_.alpn : state_.alpn;
        std::uint16_t &len = cfg_.quic != nullptr ? quic_result_.alpn_len : state_.alpn_len;
        FIBER_ASSERT(alpn.size() <= bytes.size());
        std::memcpy(bytes.data(), alpn.data(), alpn.size());
        len = static_cast<std::uint16_t>(alpn.size());
    }

    // ---- flight builders / handlers ----

    // PSK cascade over the CH's pre_shared_key offer (§2.4): structure gates
    // (fatal) → identity[0] lookup + age/suite gates (ticket reject → full
    // handshake) → binder[0] verification over the truncated ClientHello
    // (mismatch = fatal decrypt_error). `base` is the pre-CH transcript the
    // binder rides (null = CH1's plain truncated hash; CH2 rides the
    // message_hash‖HRR prefix forked off t13_ before CH2 is fed). On accept:
    // psk_accepted_/resumed_*/psk_dhe_ke_offered_ set; `alert` carries the
    // fatal alert on Fatal.
    enum class PskOutcome : std::uint8_t { Reject, Accept, Fatal };
    PskOutcome try_accept_psk(const TlsClientHello &ch, std::span<const std::uint8_t> body, const TlsTranscript13 *base,
                              TlsAlertDesc &alert) noexcept;

    // Binder[0] verification over the truncated ClientHello: local schedule
    // (binder_key is once-per-instance) over `suite_` with `psk` — the only
    // PSK re-check a CH2 owes (RFC 8446 §4.1.4: the binder covers
    // message_hash‖HRR‖Truncate(CH2)). `base` is the pre-CH transcript (null
    // = CH1's empty prefix). Fatal alert out on structure/MAC mismatch.
    [[nodiscard]] bool verify_psk_binder(TlsCipherSuiteId suite, std::span<const std::uint8_t> psk,
                                         const TlsClientHello &ch, std::span<const std::uint8_t> body,
                                         const TlsTranscript13 *base, TlsAlertDesc &alert) noexcept;

    void send_hello_retry() noexcept; // HRR shell + compat CCS → WaitClientHello2
    void handle_client_hello2(std::span<const std::uint8_t> body) noexcept;
    void send_server_flight(const TlsClientHello &ch, std::span<const std::uint8_t> z, bool after_hrr) noexcept;
    void handle_client_certificate_13(std::span<const std::uint8_t> body) noexcept; // mTLS
    void handle_client_certificate_verify_13(std::span<const std::uint8_t> body) noexcept; // mTLS
    void handle_end_of_early_data(std::span<const std::uint8_t> body) noexcept;
    void handle_client_finished(std::span<const std::uint8_t> body) noexcept;
    void finish_1_3() noexcept; // resumption master + NST + app read swap + Done
};

} // namespace fiber::tls

#endif // FIBER_TLS_HANDSHAKE_TLS13_SERVER_HANDSHAKE_H
