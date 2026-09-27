#ifndef FIBER_TLS_HANDSHAKE_TLS12_CLIENT_HANDSHAKE_H
#define FIBER_TLS_HANDSHAKE_TLS12_CLIENT_HANDSHAKE_H

// TLS 1.2 client sub-flow (06 §4.3) — internal to the client engine. Mounted
// at the ServerHello read point when the SH carries no supported_versions
// extension (the CH offered [0x0304, 0x0303], so a conforming 1.2-only peer
// lands here): SH validation, Cert/SKE/CertReq/SHD, the client flight
// ([Cert [CV]] CKE CCS Fin), NST tolerance, the server CCS read-cipher swap,
// and the finish that fills TlsConnectedState. The 1.2 side never sees the
// PSK/0-RTT state — a 1.2 fork kills the offer at the read point in the
// outer shell.
//
// Mounted by value into the outer's std::variant — never moved, never
// copied. Long-lived state is BORROWED through Mount; only flow state is
// owned here. Failures go through fail(), which encodes the fatal alert
// into the context and writes the shared outcome.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../detail/TlsHandshakeContext.h"
#include "TlsClientHandshakeShared.h"
#include "TlsTranscript.h"

#include <fiber/tls/TlsConnectedState.h>
#include <fiber/tls/TlsTypes.h>
#include <fiber/tls/crypto/Tls12KeySchedule.h>
#include <fiber/tls/handshake/TlsHandshakeCodec.h>
#include <fiber/tls/handshake/TlsHandshakeMessage.h>

namespace fiber::tls {

class Tls12ClientHandshake final : public common::NonCopyable, public common::NonMovable {
public:
    // Borrowed from the outer engine — every member outlives the sub.
    struct Mount {
        TlsHandshakeContext &ctx; // shared record pipeline
        const TlsClientConfig &cfg;
        TlsClientHelloState &hello; // transcript seed + the CKE exchange (SKE may rebuild it)
        TlsClientHandshakeOutcome &out; // terminal channel
        std::span<std::uint8_t> scratch; // client-flight staging (outer array)
    };

    explicit Tls12ClientHandshake(const Mount &mount) noexcept;
    ~Tls12ClientHandshake(); // wipes master/key_block/z

    // Entry point: the raw 1.2-shaped ServerHello (borrowed ctx bytes).
    void start(const TlsServerHello &sh, std::span<const std::uint8_t> body) noexcept;

    // Post-fork inbound traffic.
    void on_message(TlsHandshakeType type, std::span<const std::uint8_t> body) noexcept;
    // The server's CCS: swaps the read side onto the key_block server keys.
    void on_ccs() noexcept;

    // Requires done && !failed (the outer's take_state asserts it).
    [[nodiscard]] TlsConnectedState take_state() noexcept { return std::move(state_); }

private:
    // SH(1.2) → Cert → SKE → [CertReq] → SHD → client flight → [NST] →
    // server CCS → server Fin.
    enum class St : std::uint8_t {
        ExpectServerCert12,
        ExpectSke12,
        ExpectCrShd12,
        ExpectServerCcs12,
        ExpectServerFin12,
        Done,
    };

    // ---- borrowed (Mount) ----
    TlsHandshakeContext &ctx_;
    const TlsClientConfig &cfg_;
    TlsClientHelloState &hello_;
    TlsClientHandshakeOutcome &out_;
    std::span<std::uint8_t> scratch_;

    // ---- owned flow state ----
    St st_ = St::ExpectServerCert12;
    TlsTranscript12 t12_;
    std::array<std::uint8_t, 64> hash_buf_{};
    TlsCipherSuiteId suite_ = TlsCipherSuiteId::TlsAes128GcmSha256;
    std::array<std::uint8_t, 32> server_random_{}; // copied at the SH(1.2) read point
    TlsKxShared z12_{};
    TlsSecret master12_{};
    Tls12WriteKeys kb12_{}; // key_block; wiped once both ciphers hold the material
    bool ems_negotiated_ = false;
    bool cr12_received_ = false;
    bool sent_cert12_ = false; // we sent a (non-empty) client Certificate
    std::array<std::uint16_t, kClientMaxCrSigalgs> cr_sigalgs_{};
    std::size_t cr_sigalgs_n_ = 0; // our schemes the CR lists (tls_client_keep_cr_sigalgs)
    bool cr_sigalgs_present_ = false; // the CR carried a list at all
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

    void feed12(TlsHandshakeType type, std::span<const std::uint8_t> body) noexcept {
        const std::array<std::uint8_t, 4> header{static_cast<std::uint8_t>(type), 0,
                                                 static_cast<std::uint8_t>(body.size() >> 8),
                                                 static_cast<std::uint8_t>(body.size())};
        (void) t12_.update(header);
        (void) t12_.update(body);
    }

    void snapshot12() noexcept { (void) t12_.snapshot_digest({hash_buf_.data(), hash_len()}); }

    [[nodiscard]] bool emit_message(std::span<const std::uint8_t> msg) noexcept {
        return ctx_.emit(TlsContentType::Handshake, msg).has_value();
    }

    // The 1.2 key_block handoff: a fresh cipher instance over one direction's
    // sliced material (write side at our CCS, read side at the server's). The
    // cipher copies what it needs; kb12_ is wiped once BOTH directions hold it.
    [[nodiscard]] bool swap_cipher_12(TlsRecordCipher &slot, const TlsTrafficKeys &keys) noexcept {
        TlsRecordCipher fresh;
        if (!fresh.init(suite_, TlsRecordProtectionKind::Tls12, {keys.key.data(), keys.key_len},
                        {keys.iv.data(), keys.iv_len})
                     .has_value()) {
            return false;
        }
        slot = std::move(fresh);
        return true;
    }

    void wipe_kb12() noexcept {
        tls_secure_wipe(kb12_.client.key.data(), kb12_.client.key.size());
        tls_secure_wipe(kb12_.client.iv.data(), kb12_.client.iv.size());
        tls_secure_wipe(kb12_.server.key.data(), kb12_.server.key.size());
        tls_secure_wipe(kb12_.server.iv.data(), kb12_.server.iv.size());
    }

    // ---- message handlers ----

    void handle_certificate_12(std::span<const std::uint8_t> body) noexcept;
    void handle_server_key_exchange_12(std::span<const std::uint8_t> body) noexcept;
    void handle_certificate_request_12(std::span<const std::uint8_t> body) noexcept;
    void handle_server_hello_done_12(std::span<const std::uint8_t> body) noexcept;
    [[nodiscard]] bool send_client_flight_12() noexcept; // [Cert [CV]] CKE CCS Fin (raises its own alerts)
    void handle_new_session_ticket_12(std::span<const std::uint8_t> body) noexcept;
    void handle_finished_12(std::span<const std::uint8_t> body) noexcept;
    void finish_1_2() noexcept; // Done
};

} // namespace fiber::tls

#endif // FIBER_TLS_HANDSHAKE_TLS12_CLIENT_HANDSHAKE_H
