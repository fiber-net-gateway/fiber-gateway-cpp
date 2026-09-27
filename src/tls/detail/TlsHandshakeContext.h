#ifndef FIBER_TLS_DETAIL_TLS_HANDSHAKE_CONTEXT_H
#define FIBER_TLS_DETAIL_TLS_HANDSHAKE_CONTEXT_H

// Shared byte-plumbing root for the handshake engines (06 §5.1; internal to
// src/tls — the engines stay thin FSMs and 07 reuses this). This layer owns
// record I/O mechanics only; every protocol decision — which message is
// expected next, when transcripts are fed and snapshotted, when cipher
// instances swap — belongs to the engine:
//
//   inbound   reader framing → record routing (decrypt / alert / CCS surface)
//             → handshake reassembly (per-type caps, below) → one complete message per
//             step() as a borrowed span
//   outbound  emit() (plaintext via the Writer, sealed per-record chunk),
//             send_ccs(), fatal-alert encoding, out_ accumulation
//
// Transcripts deliberately live in the ENGINE, not here: snapshot ordering
// (CertificateVerify/Finished hash BEFORE the covering message is fed) is
// protocol state, and keeping the feed points next to their snapshot points
// in the FSM makes the ordering auditable (06 §4.2 tables).
//
// Node pool contract: chains resolve the current loop's node pool per
// operation (see IoBufChain) — the context and its callers live on the
// connection's loop. No OpenSSL include here — record protection goes
// through TlsRecordCipher.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <fiber/common/IoError.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/common/mem/IoBuf.h>
#include <fiber/common/mem/IoBufChain.h>
#include <fiber/tls/TlsConfig.h>
#include <fiber/tls/TlsTypes.h>
#include <fiber/tls/handshake/TlsHandshakeMessage.h>
#include <fiber/tls/record/TlsRecord.h>
#include <fiber/tls/record/TlsRecordCipher.h>
#include <fiber/tls/record/TlsRecordReader.h>
#include <fiber/tls/record/TlsRecordWriter.h>

namespace fiber::tls {

// Which protection state the INBOUND stream is in. A mode flag rather than
// "cipher active?" because the two disagree in TLS 1.2: after the client's
// own CCS its write cipher is live while server records stay plaintext until
// the server's CCS arrives.
enum class TlsInboundMode : std::uint8_t {
    Plaintext13, // pre-ServerHello 1.3: plaintext records; app_data fatal; ≤4 handshake records
    Plaintext12, // 1.2 before the server's CCS: plaintext records, app_data fatal
    Sealed13, // 1.3 after ServerHello: outer app_data only (opened, inner type routed)
    Sealed12, // 1.2 after the server's CCS: outer handshake/alert sealed, types preserved
};

// One inbound step result. `body` (Message) borrows context storage and is
// valid until the next step() call. Fatal carries the alert to SEND; the
// caller invokes fail() (or fail() may already have run — idempotent).
struct TlsInboundStep {
    enum class Kind : std::uint8_t { NeedMore, Message, Ccs, Alert, Fatal };
    Kind kind = Kind::NeedMore;
    TlsAlertDesc alert = TlsAlertDesc::CloseNotify; // Fatal: ours to send / Alert: the peer's
    TlsHandshakeType type = TlsHandshakeType::ClientHello; // Message
    std::span<const std::uint8_t> body{}; // Message
    bool close_notify = false; // Alert: desc == close_notify (peer closed mid-handshake)
};

class TlsHandshakeContext : public common::NonCopyable, public common::NonMovable {
public:
    // RFC 8446 §4.1.2 DOs limits, 06 §2.6: bounded pre-key handshake records
    // (1.3) and reassembled message size (both versions).
    static constexpr std::size_t kMaxPlaintextHandshakeRecords13 = 4;
    // Declared body-length caps, checked on the 4-byte header before any
    // body byte is buffered — a server's unauthenticated peer can pin at
    // most this much reassembly memory per connection. Parity with
    // BoringSSL: kMaxMessageLen (16 KiB) for ordinary messages and
    // SSL_MAX_CERT_LIST_DEFAULT (100 KiB) for certificate chains. The
    // client engine raises the ordinary cap to the chain cap (a 1.2
    // CertificateRequest may carry a long certificate_authorities list —
    // BoringSSL's client allows max_cert_list for every in-handshake
    // message).
    static constexpr std::size_t kMaxHandshakeMessage = 16u << 10;
    static constexpr std::size_t kMaxCertificateMessage = 100u << 10;
    static constexpr std::size_t kOpenScratchSize = kTlsMaxCiphertextRecordSize;

    TlsHandshakeContext() noexcept;
    ~TlsHandshakeContext();

    // ---- inbound ----

    // Appends peer bytes; false only on allocation failure (feed reports it
    // as NoMem — a connection-level failure).
    [[nodiscard]] bool feed(mem::IoBuf &&bytes) noexcept;
    [[nodiscard]] bool feed(mem::IoBufChain &&bytes) noexcept;

    // QUIC mode (10 §3.3): appends CRYPTO-stream bytes — raw handshake
    // messages, NO record framing — at `level`. Ordering is the QUIC layer's
    // gate contract (it feeds Initial then Handshake then Application, fully
    // consuming each level's stream in between); the context only tracks
    // monotonicity as a contract check. False only on allocation failure.
    // Mutually exclusive with feed().
    [[nodiscard]] bool provide_quic(TlsQuicLevel level, std::span<const std::uint8_t> bytes) noexcept;

    void set_inbound_mode(TlsInboundMode mode) noexcept { mode_ = mode; }

    // Ordinary (non-Certificate) message cap; default kMaxHandshakeMessage.
    // Certificate messages are always capped at kMaxCertificateMessage.
    void set_max_handshake_message(std::size_t limit) noexcept { max_message_ = limit; }
    [[nodiscard]] TlsInboundMode inbound_mode() const noexcept { return mode_; }

    // Pulls the next protocol event: a complete handshake message (possibly
    // reassembled across records), a validated 0x01 CCS, a decoded peer
    // alert, NeedMore, or Fatal (record-level violation; alert not yet
    // encoded — call fail()). In QUIC mode: complete messages off the CRYPTO
    // stream only (no alerts/CCS — those do not exist without records).
    [[nodiscard]] TlsInboundStep step() noexcept;

    // ---- QUIC mode (10 §3): enable before first use; all four callbacks
    // must be present. Thereafter feed() is invalid, records never frame,
    // emit() sinks to add_handshake_data, send_ccs() is suppressed, and
    // fail() reports via send_alert instead of encoding bytes. ----

    void enable_quic(const TlsQuicCallbacks &callbacks) noexcept;
    [[nodiscard]] bool quic() const noexcept { return quic_ != nullptr; }

    // Advances the outbound CRYPTO level (the engine sets it at the flight
    // boundaries: client CH→Initial then Handshake at the hs secrets;
    // server HRR→Initial then Handshake, Application at the app secrets).
    // Monotonic by construction — asserted.
    void set_quic_level(TlsQuicLevel level) noexcept;

    // ---- outbound ----

    // Legacy_record_version for PLAINTEXT records (sealed records are always
    // 0x0303, matching the cipher's AAD). The engine sets 0x0301 for the
    // first flight and 0x0303 after the ServerHello (06 §2.3).
    void set_legacy_version(std::uint16_t version) noexcept { writer_.set_legacy_version(version); }

    // Frames `payload` as records of `type` into out_. Plaintext while no
    // write cipher is active (via the Writer: zero framing copies); sealed
    // per-record chunk otherwise, or with `cipher` when given (the 0-RTT
    // early-write instance). Sealed records are written as outer
    // application_data with legacy_version 0x0303. QUIC mode: the payload
    // (one fully-encoded handshake message) goes to add_handshake_data at
    // the current outbound level — type and cipher are ignored, nothing
    // lands in out_.
    [[nodiscard]] common::IoResult<void> emit(TlsContentType type, std::span<const std::uint8_t> payload,
                                              TlsRecordCipher *cipher = nullptr) noexcept;

    // 1-byte 0x01 ChangeCipherSpec — always plaintext (by definition).
    // QUIC mode: suppressed (RFC 9001 §5.3 forbids the compat CCS) — a
    // successful no-op.
    [[nodiscard]] common::IoResult<void> send_ccs() noexcept;

    // ---- record-protection swap points (engine-driven) ----

    [[nodiscard]] TlsRecordCipher &read_cipher() noexcept { return read_cipher_; }
    [[nodiscard]] TlsRecordCipher &write_cipher() noexcept { return write_cipher_; }

    // ---- 0-RTT early data (RFC 8446 §4.2.10; engine-driven windows) ----

    // Sink: the read cipher IS the early instance (0-RTT accepted). Inner
    // application_data content is decrypted, budget-checked in PLAINTEXT
    // content bytes (kMaxEarlyDataAccepted; over → unexpected_message,
    // RFC 8446 §4.6.1), and copied into `out`. The window ends when the
    // engine disarms at EndOfEarlyData.
    void arm_early_data_sink(mem::IoBufChain &out, std::size_t budget) noexcept {
        early_sink_ = &out;
        early_budget_ = budget;
        early_used_ = 0;
        early_sink_armed_ = true;
    }

    // Skip: 0-RTT rejected but the client may still have early records in
    // flight. In Sealed13 auth-failing outer application_data records are
    // silently discarded (budget counts CIPHERTEXT bytes — the plaintext is
    // unknowable — kMaxEarlyDataSkipped); the first successful open is the
    // start of the client's second flight and ends the window. In Plaintext13
    // (post-HRR, awaiting CH2) outer application_data records are skipped by
    // type alone — no key exists to try — and the first handshake record ends
    // the window.
    void arm_early_data_skip(std::size_t budget) noexcept {
        early_sink_ = nullptr;
        early_budget_ = budget;
        early_used_ = 0;
        early_skip_armed_ = true;
    }

    void disarm_early_data() noexcept {
        early_sink_armed_ = false;
        early_skip_armed_ = false;
        early_sink_ = nullptr;
    }

    // ---- connected phase handoff ----

    // Inbound bytes fed past the terminal event but never consumed (app
    // data piggybacked behind the final flight): handed to the glue's
    // connected-phase record reader. A second take yields an empty chain.
    // QUIC mode: the unconsumed CRYPTO-stream tail (e.g. an NST that
    // arrived in the same provide) — the post-handshake consumer's input.
    [[nodiscard]] mem::IoBufChain take_inbound_leftover() noexcept {
        return quic_ != nullptr ? std::move(reassembly_) : reader_.take_pending();
    }

    // Sink-window content bytes handed over so far (tests/diagnostics).
    [[nodiscard]] std::size_t early_data_received() const noexcept { return early_sink_armed_ ? early_used_ : 0; }

    // ---- failure ----

    // Encodes a fatal alert into out_ (sealed when the write cipher is
    // active) and marks the context failed. Idempotent; NoMem while encoding
    // leaves failed_ set without bytes (connection dies either way).
    // QUIC mode: reports via send_alert — no bytes are ever encoded.
    void fail(TlsAlertDesc desc) noexcept;
    [[nodiscard]] bool failed() const noexcept { return failed_; }
    [[nodiscard]] TlsAlertDesc failure_alert() const noexcept { return failure_alert_; }

    // ---- output ----

    // Hands the accumulated outbound bytes over; a second take yields an
    // empty chain.
    [[nodiscard]] mem::IoBufChain take_output() noexcept;

    // Bytes buffered awaiting framing (reader + current record + reassembly);
    // for tests and diagnostics.
    [[nodiscard]] std::size_t pending_bytes() const noexcept;

private:
    [[nodiscard]] TlsInboundStep take_record(TlsRecord &&record) noexcept;
    [[nodiscard]] TlsInboundStep route_current(TlsRecord &&record) noexcept;
    [[nodiscard]] TlsInboundStep open_current(TlsRecord &record) noexcept;
    [[nodiscard]] bool append_fragment(std::span<const std::uint8_t> bytes) noexcept;
    [[nodiscard]] TlsInboundStep extract_message() noexcept;
    [[nodiscard]] TlsInboundStep quic_step() noexcept;
    [[nodiscard]] std::size_t max_body_len(std::uint8_t type) const noexcept;

    mem::IoBufChain out_{};
    mem::IoBufChain reassembly_{};
    TlsRecord current_{}; // record whose plaintext bytes are being consumed
    mem::IoBuf message_buf_{}; // materialized straddling message (owns the body span)
    TlsRecordReader reader_{};
    TlsRecordWriter writer_{};
    TlsRecordCipher read_cipher_{};
    TlsRecordCipher write_cipher_{};
    TlsInboundMode mode_ = TlsInboundMode::Plaintext13;
    TlsAlertDesc failure_alert_ = TlsAlertDesc::InternalError;
    const std::uint8_t *plain_ = nullptr; // current handshake-plaintext span (record bytes or scratch)
    std::size_t current_off_ = 0; // bytes of plain_[0..plain_len_) already consumed
    std::size_t plain_len_ = 0;
    std::size_t plaintext_records_13_ = 0;
    std::size_t max_message_ = kMaxHandshakeMessage;
    bool has_current_ = false;
    bool failed_ = false;
    // ---- 0-RTT window state (see the arm/disarm API above) ----
    mem::IoBufChain *early_sink_ = nullptr; // sink target while armed
    std::size_t early_budget_ = 0; // content (sink) / ciphertext (skip) ceiling
    std::size_t early_used_ = 0;
    bool early_sink_armed_ = false;
    bool early_skip_armed_ = false;
    // ---- QUIC mode (see enable_quic) ----
    const TlsQuicCallbacks *quic_ = nullptr; // null = TCP record mode
    TlsQuicLevel quic_level_ = TlsQuicLevel::Initial; // outbound CRYPTO level
    TlsQuicLevel quic_provided_level_ = TlsQuicLevel::Initial; // inbound gate contract check
    // In-place-open / materialization scratch; also the straddle destination
    // for tls_record_open_in_place.
    std::array<std::uint8_t, kOpenScratchSize> open_scratch_{};
};

} // namespace fiber::tls

#endif // FIBER_TLS_DETAIL_TLS_HANDSHAKE_CONTEXT_H
