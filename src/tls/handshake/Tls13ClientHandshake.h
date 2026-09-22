#ifndef FIBER_TLS_HANDSHAKE_TLS13_CLIENT_HANDSHAKE_H
#define FIBER_TLS_HANDSHAKE_TLS13_CLIENT_HANDSHAKE_H

// TLS 1.3 client sub-flow (06 §4.2) — internal to the client engine. The
// outer TlsClientHandshakeEngine builds and sends the ClientHello (± PSK
// binder ± 0-RTT window), detects the version at the ServerHello read point,
// and hands the raw message to one sub-flow; this one owns everything
// 1.3-specific from there: the HRR loop (CH2 rebuild), the PSK outcome, the
// server flight (EE/Cert/CV/CertReq/Fin), EOED, and the finish that fills
// TlsConnectedState.
//
// Mounted by value into the outer's std::variant — never moved, never
// copied. Everything long-lived is BORROWED through Mount (the outer and its
// buffers outlive the sub); only flow state is owned here. Failures go
// through fail(), which encodes the fatal alert into the context and writes
// the shared outcome — no return-value plumbing.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "../detail/TlsHandshakeContext.h"
#include "TlsClientHandshakeShared.h"
#include "TlsTranscript.h"

#include <fiber/tls/TlsConnectedState.h>
#include <fiber/tls/TlsTypes.h>
#include <fiber/tls/handshake/TlsHandshakeCodec.h>
#include <fiber/tls/handshake/TlsHandshakeMessage.h>

namespace fiber::tls {

class Tls13ClientHandshake final : public common::NonCopyable, public common::NonMovable {
public:
    // Borrowed from the outer engine — every member outlives the sub.
    struct Mount {
        TlsHandshakeContext &ctx; // shared record pipeline
        const TlsClientConfig &cfg;
        const TlsSessionOffer *session; // nullptr = no PSK offered
        TlsClientHelloState &hello; // mutable: HRR rewrites CH2 in place
        std::optional<TlsKeySchedule13> &sched; // PSK binder tree; reset/continued here
        TlsClientEarlyWindow &early; // 0-RTT window (closes at SH/HRR/EOED)
        TlsClientHandshakeOutcome &out; // terminal channel
        std::span<std::uint8_t> scratch; // client-flight staging (outer array)
        bool psk_offered;
        bool ccs_sent; // the compat CCS already went out with the early flight
    };

    explicit Tls13ClientHandshake(const Mount &mount) noexcept;
    ~Tls13ClientHandshake(); // wipes traffic/resumption secrets

    // Entry points: the first inbound message (a real ServerHello, or the
    // HRR-shaped one). `body` is the complete raw message (borrowed ctx
    // bytes, valid for the call).
    void start(const TlsServerHello &sh, std::span<const std::uint8_t> body) noexcept;
    void start_hello_retry_request(const TlsServerHello &sh, std::span<const std::uint8_t> body) noexcept;

    // Post-fork inbound traffic. WaitServerHello only recurs after an HRR
    // (the second message is the real SH; a second HRR is a MUST-abort).
    void on_message(TlsHandshakeType type, std::span<const std::uint8_t> body) noexcept;
    void on_ccs() noexcept {
        // The 1.3 client ignores every well-formed peer CCS (middlebox
        // compat, including the server's post-SH one); the context already
        // validated the 1-byte 0x01 form.
    }

    // Requires done && !failed (the outer's take_state asserts it).
    [[nodiscard]] TlsConnectedState take_state() noexcept { return std::move(state_); }

private:
    enum class St : std::uint8_t {
        WaitServerHello, // an HRR was processed; the real SH is still owed
        ExpectEe,
        ExpectCrCertFin,
        ExpectCv,
        ExpectFin,
        Done,
    };

    // ---- borrowed (Mount) ----
    TlsHandshakeContext &ctx_;
    const TlsClientConfig &cfg_;
    const TlsSessionOffer *session_;
    TlsClientHelloState &hello_;
    std::optional<TlsKeySchedule13> &sched_;
    TlsClientEarlyWindow &early_;
    TlsClientHandshakeOutcome &out_;
    std::span<std::uint8_t> scratch_;
    const bool psk_offered_;
    bool ccs_sent_;

    // ---- owned flow state ----
    St st_ = St::WaitServerHello;
    TlsTranscript13 t13_;
    std::array<std::uint8_t, 64> hash_buf_{};
    TlsSecret client_hs_{};
    TlsSecret server_hs_{};
    TlsSecret client_app0_{};
    TlsSecret server_app0_{};
    TlsSecret resumption_master_{};
    TlsCipherSuiteId suite_ = TlsCipherSuiteId::TlsAes128GcmSha256;
    std::uint8_t hrr_count_ = 0;
    TlsCipherSuiteId hrr_suite_ = TlsCipherSuiteId::TlsAes128GcmSha256; // valid when hrr_count_ == 1
    bool psk_accepted_ = false;
    bool ee_early_data_ = false;
    bool cr13_received_ = false;
    std::array<std::uint16_t, kClientMaxCrSigalgs> cr_sigalgs_{};
    std::size_t cr_sigalgs_n_ = 0;
    std::array<std::uint8_t, 255> cr_context_{};
    std::size_t cr_ctx_len_ = 0;
    TlsCertificateChain peer_chain_;
    TlsConnectedState state_{};

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
    // into `slot` — re-init of a live instance is a 05 contract violation;
    // every key change is an instance swap (the 1.3 app instances restart at
    // seq 0 while the hs instances' sequence dies with the flight).
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

    // ---- message handlers ----

    void handle_server_hello(std::span<const std::uint8_t> body) noexcept; // post-HRR re-entry
    void handle_encrypted_extensions(std::span<const std::uint8_t> body) noexcept;
    void handle_certificate_request_13(std::span<const std::uint8_t> body) noexcept;
    void handle_certificate_13(std::span<const std::uint8_t> body) noexcept;
    void handle_certificate_verify_13(std::span<const std::uint8_t> body) noexcept;
    void handle_finished_13(std::span<const std::uint8_t> body) noexcept;
    [[nodiscard]] bool send_client_certificate_13() noexcept; // Cert + CV when credential present
    void finish_1_3() noexcept; // remaining client flight + Done
};

} // namespace fiber::tls

#endif // FIBER_TLS_HANDSHAKE_TLS13_CLIENT_HANDSHAKE_H
