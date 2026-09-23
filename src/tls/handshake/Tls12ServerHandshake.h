#ifndef FIBER_TLS_HANDSHAKE_TLS12_SERVER_HANDSHAKE_H
#define FIBER_TLS_HANDSHAKE_TLS12_SERVER_HANDSHAKE_H

// TLS 1.2 server sub-flow (07 §4.3) — internal to the server engine. The
// outer TlsServerHandshakeEngine forks here when the ClientHello offers
// 0x0303 and not 0x0304: the SH flight (SH [CertReq] Cert SKE SHD), the
// client flight ([Cert] CKE [CV]) plaintext, the client-CCS read swap, the
// client Finished check, and the final flight ([NST] CCS server Fin).
// Always a full handshake — 1.2 resumption belongs to 08 (§10.7).
//
// Mounted by value into the outer's std::variant — never moved, never
// copied. Long-lived state is BORROWED through Mount; only flow state is
// owned here. Failures go through fail(), which encodes the fatal alert
// into the context and writes the shared outcome — no return-value
// plumbing.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../detail/TlsHandshakeContext.h"
#include "TlsServerHandshakeShared.h"
#include "TlsTranscript.h"

#include <fiber/tls/TlsConnectedState.h>
#include <fiber/tls/TlsTypes.h>
#include <fiber/tls/crypto/Tls12KeySchedule.h>
#include <fiber/tls/handshake/TlsHandshakeCodec.h>
#include <fiber/tls/handshake/TlsHandshakeMessage.h>

namespace fiber::tls {

class Tls12ServerHandshake final : public common::NonCopyable, public common::NonMovable {
public:
    // Borrowed from the outer engine — every member outlives the sub.
    struct Mount {
        TlsHandshakeContext &ctx; // shared record pipeline
        const TlsServerConfig &cfg;
        const TlsTicketMinter *minter; // nullptr = never mint an NST
        TlsServerHelloState &hello; // retained CH + the SKE exchange + server_random
        TlsServerHandshakeOutcome &out; // terminal channel
        std::span<std::uint8_t> scratch; // flight staging (outer array)
    };

    explicit Tls12ServerHandshake(const Mount &mount) noexcept;
    ~Tls12ServerHandshake(); // wipes master/key_block/z

    // Entry point: the first inbound ClientHello. `ch` is the decoded view
    // (borrowing hello.ch — stable) and `body` the raw message body over the
    // same retained storage (no 4-byte header; feed12 reconstructs it).
    void start(const TlsClientHello &ch, std::span<const std::uint8_t> body) noexcept;

    // Post-fork inbound traffic.
    void on_message(TlsHandshakeType type, std::span<const std::uint8_t> body) noexcept;
    // The client's CCS: swaps the read side onto the key_block client keys.
    void on_ccs() noexcept;

    // Requires done && !failed (the outer's take_state asserts it).
    [[nodiscard]] TlsConnectedState take_state() noexcept { return std::move(state_); }

private:
    // RFC 5246 §7.3 client flight order: Certificate, ClientKeyExchange,
    // CertificateVerify — the CV signs the transcript THROUGH the CKE.
    enum class St : std::uint8_t {
        ExpectClientCert12, // mTLS: a CR went out; the client flight opens with its Cert
        ExpectCke12,
        ExpectClientCv12, // mTLS: CV over the raw transcript through CKE
        ExpectClientCcs12,
        ExpectClientFin12,
        Done,
    };

    // ---- borrowed (Mount) ----
    TlsHandshakeContext &ctx_;
    const TlsServerConfig &cfg_;
    const TlsTicketMinter *minter_;
    TlsServerHelloState &hello_;
    TlsServerHandshakeOutcome &out_;
    std::span<std::uint8_t> scratch_;

    // ---- owned flow state ----
    St st_ = St::ExpectCke12;
    TlsTranscript12 t12_;
    std::array<std::uint8_t, 64> hash_buf_{};
    TlsCipherSuiteId suite_ = TlsCipherSuiteId::TlsAes128GcmSha256;
    TlsKxShared z12_{};
    TlsSecret master12_{};
    Tls12WriteKeys kb12_{}; // key_block; wiped once both ciphers hold the material
    bool ems_negotiated_ = false;
    bool ticket_wanted_ = false; // the CH offered 5077 AND a minter is wired
    bool peer_sent_cert12_ = false; // the client's Certificate was non-empty
    std::array<std::uint8_t, 1024> ticket_buf_{}; // the minter's staging (NST body)
    TlsCertificateChain peer_chain_; // the mTLS client chain (empty otherwise)
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

    // Feeds a ctx-borrowed message BODY (no header) into the transcript —
    // the header is reconstructed from (type, body.size()).
    void feed12(TlsHandshakeType type, std::span<const std::uint8_t> body) noexcept {
        const std::array<std::uint8_t, 4> header{static_cast<std::uint8_t>(type), 0,
                                                 static_cast<std::uint8_t>(body.size() >> 8),
                                                 static_cast<std::uint8_t>(body.size())};
        (void) t12_.update(header);
        (void) t12_.update(body);
    }

    void snapshot12() noexcept { (void) t12_.snapshot_digest({hash_buf_.data(), hash_len()}); }

    // Emits a fully-encoded handshake message (header+body in `msg`); the
    // context seals it when the write cipher is live. False = connection-level
    // failure already encoded.
    [[nodiscard]] bool emit_message(std::span<const std::uint8_t> msg) noexcept {
        return ctx_.emit(TlsContentType::Handshake, msg).has_value();
    }

    // The 1.2 key_block handoff: a fresh cipher instance over one direction's
    // sliced material (write side at our CCS, read side at the client's). The
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

    // ---- flight builders / handlers ----

    void send_server_flight_12(const TlsClientHello &ch) noexcept; // SH [CR] Cert SKE SHD
    void handle_client_certificate_12(std::span<const std::uint8_t> body) noexcept; // mTLS
    void handle_client_key_exchange_12(std::span<const std::uint8_t> body) noexcept; // z + master + keys
    void handle_client_certificate_verify_12(std::span<const std::uint8_t> body) noexcept; // mTLS
    void handle_client_finished_12(std::span<const std::uint8_t> body) noexcept;
    [[nodiscard]] bool send_final_flight_12() noexcept; // [NST] CCS Fin (raises its own alerts)
    void finish_1_2() noexcept; // Done
};

} // namespace fiber::tls

#endif // FIBER_TLS_HANDSHAKE_TLS12_SERVER_HANDSHAKE_H
