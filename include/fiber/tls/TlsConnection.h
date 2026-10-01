#ifndef FIBER_TLS_TLS_CONNECTION_H
#define FIBER_TLS_TLS_CONNECTION_H

// Connected-phase record engine (feature/tls/09 §3; framing decoupled from
// the connection): takes a TlsConnectedState by move from either handshake
// engine at HandshakeDone and drives everything that flies after the
// handshake — application data, alerts, and the TLS 1.3 post-handshake
// messages we handle:
//   NewSessionTicket  swallowed (09 §1: no client session cache)
//   KeyUpdate         read side rekeys on receipt; update_requested arms the
//                     passive RFC 8446 §4.6.1 response (own KeyUpdate under
//                     the CURRENT write keys, rotation after) which goes out
//                     before the next write/close_notify
//   anything else     fatal unexpected_message (1.2 HelloRequest included —
//                     renegotiation is refused outright, 09 §1)
// We never initiate a KeyUpdate ourselves (09 §1).
//
// Record framing lives OUTSIDE (the 09 §3 retrofit of TlsHandshakeContext is
// gone; feature/tls/12): the glue (TlsStreamFd) keeps the inbound byte
// stream in one contiguous wire buffer, frames complete records off it with
// tls_frame_records() and hands them over in batches to on_records(); an
// incomplete trailing record stays with the glue across reads. Every record
// is therefore contiguous: the connection opens it in place inside the wire
// buffer and delivers app-data plaintext as retained slices of that buffer —
// zero copies, no scratch. Post-handshake handshake messages (NST/KeyUpdate)
// reassemble across records here (16 KiB per-message cap).
//
// Synchronous, memory-only plumbing — the net glue (TlsStreamFd, 09 §5)
// owns the socket loop: fd bytes → its wire buffer → on_records(),
// take_output() → fd. Pure writes (write/close_notify) are fail-fast; inbound violations
// latch a terminal state and encode the fatal alert into the outbound chain
// (the glue flushes it best-effort before tearing down). The peer's
// close_notify latches PeerClosed — plaintext delivered before it stays
// readable.
//
// Node pool semantics as everywhere else: chains resolve the current
// loop's node pool per operation — run/destroy the connection on the
// connection's loop. All state is held by value (two record ciphers, the
// rekey secrets, three chains — ~1.8 KB, no allocation of its own), so the
// glue embeds the connection directly.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <fiber/common/IoError.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/common/mem/IoBuf.h>
#include <fiber/common/mem/IoBufChain.h>

#include "TlsConnectedState.h"
#include "TlsTypes.h"
#include "handshake/TlsHandshakeMessage.h"
#include "record/TlsRecordFramer.h"

namespace fiber::tls {

enum class TlsConnectionRole : std::uint8_t { Client, Server };

class TlsConnection final : public common::NonCopyable, common::NonMovable {
public:
    // read()/take() outcome. Ok always delivered > 0 bytes; NeedMore means
    // no complete record yet (feed more); PeerClosed/Fatal are latched
    // terminals (pending plaintext drains first). NoMem is take()-only: a
    // node-split allocation failed with the plaintext untouched.
    enum class ReadStatus : std::uint8_t { Ok, NeedMore, PeerClosed, Fatal, NoMem };

    // Moves the ciphers and KeyUpdate secrets out of `state` (which is left
    // emptied). The version picks the dispatch table; the role picks which
    // app secret is ours (write-side rekey base) vs the peer's.
    TlsConnection(TlsConnectionRole role, TlsConnectedState &&state) noexcept;

    // ---- inbound ----

    // Complete records framed off one contiguous wire buffer
    // (tls_frame_records), in wire order; offsets are relative to
    // wire.readable_data(). Each record is processed immediately: opened in
    // place inside `wire` with the read cipher (only its own bytes are
    // written, so the storage needs no unique(); auth failure → fatal
    // bad_record_mac), its inner content routed (app data delivered as a
    // retained slice of `wire`, alerts latched, post-handshake messages
    // dispatched). Processing stops at the first terminal (ours or the
    // peer's close_notify): the rest of the batch drops.
    void on_records(mem::IoBuf &wire, std::span<const TlsRecordSpan> records) noexcept;

    // Reader-level framing violation (unknown content type / oversize
    // record): latches the fatal terminal with the reader's alert.
    void on_framing_fatal(TlsAlertDesc alert) noexcept;

    // Drains delivered plaintext into buf (a memcpy API). Ok with out_len >
    // 0; once empty, the latched terminal (or NeedMore) surfaces.
    [[nodiscard]] ReadStatus read(void *buf, std::size_t len, std::size_t &out_len) noexcept;

    // Zero-copy read: moves up to `size` plaintext bytes onto the back of
    // `out` — whole nodes splice, a straddling node splits into a retained
    // view. Status order mirrors read(); a split's allocation failure
    // reports NoMem with the plaintext untouched.
    [[nodiscard]] ReadStatus take(std::size_t size, mem::IoBufChain &out, std::size_t &out_len) noexcept;

    [[nodiscard]] std::size_t pending_plaintext() const noexcept;

    // ---- outbound ----

    // Seals payload as application-data records into the outbound chain
    // (<= 16384 plaintext bytes per record; 1.3 outer application_data,
    // 1.2 type-preserved; empty payload → one zero-length record). A
    // pending KeyUpdate response goes out first. Invalid on a terminal
    // connection or after close_notify; NoMem is connection-fatal (the
    // chain may hold a prefix of the records).
    [[nodiscard]] common::IoResult<void> write(std::span<const std::uint8_t> payload) noexcept;

    // Graceful close: close_notify into the outbound chain (a pending
    // KeyUpdate response goes out first — RFC 8446 §4.6.1 MUST). The glue
    // flushes and is done — waiting for the peer's close_notify is the
    // reader's business (read() turns PeerClosed), not the closer's
    // (09 §5). Idempotent.
    [[nodiscard]] common::IoResult<void> close_notify() noexcept;

    // Outbound bytes to hand to the socket (app-data records, the KeyUpdate
    // response, alerts). A second take yields an empty chain.
    [[nodiscard]] mem::IoBufChain take_output() noexcept;

    // ---- state ----

    [[nodiscard]] bool peer_closed() const noexcept; // the peer's close_notify latched
    [[nodiscard]] bool failed() const noexcept; // fatal latched (ours or the peer's)
    [[nodiscard]] std::span<const std::uint8_t> alpn() const noexcept; // the negotiated protocol

private:
    // Post-handshake handshake-message dispatch (1.3 NST/KeyUpdate; 1.2 all
    // fatal). False = terminal latched, fragment processing stops.
    [[nodiscard]] bool dispatch_post_handshake(TlsHandshakeType type, std::span<const std::uint8_t> body) noexcept;
    [[nodiscard]] bool handle_key_update(std::span<const std::uint8_t> body) noexcept;

    // Sends the armed KeyUpdate response (RFC 8446 §4.6.1 MUST): the message
    // itself under the CURRENT write keys, the rotation applying to the
    // records after it. Everything that can fail is staged before any state
    // moves. No-op when nothing is armed.
    [[nodiscard]] common::IoResult<void> send_pending_rekey() noexcept;

    // Latches a terminal and encodes our fatal alert into the outbound
    // chain (sealed — the write cipher always lives here; the glue flushes
    // best-effort, then tears down). Idempotent; NoMem while encoding
    // leaves failed_ set without bytes (the connection dies either way).
    void latch_fatal(TlsAlertDesc alert) noexcept;

    [[nodiscard]] common::IoResult<void> write_guard() noexcept;

    // ---- inbound record pipeline ----
    // Every record is contiguous inside the glue's wire buffer (`wire`,
    // payload at wire.readable_data() + record.offset).

    // Outer-type dispatch (the mode is fixed: sealed, per version).
    void route_record(mem::IoBuf &wire, const TlsRecordSpan &record) noexcept;
    // Opens the sealed body in place and routes the inner content type. 1.2
    // preserves the outer type under encryption (the AAD-bound inner type IS
    // it); 1.3 routes the decrypted trailing type.
    void open_and_route(mem::IoBuf &wire, const TlsRecordSpan &record) noexcept;
    // Two decoded alert bytes (severity, description).
    void on_alert_bytes(const std::uint8_t *bytes) noexcept;
    // App data delivery: the opened plaintext wire[offset, offset + plain_len)
    // joins the delivery chain as a retained slice of the wire buffer — zero
    // copies.
    void deliver_plaintext(mem::IoBuf &wire, std::size_t offset, std::size_t plain_len) noexcept;
    // Post-handshake handshake-message reassembly across records, then
    // dispatch per complete message (borrowed bodies).
    void feed_handshake_fragment(const std::uint8_t *frag, std::size_t len) noexcept;
    [[nodiscard]] bool append_fragment(std::span<const std::uint8_t> bytes) noexcept;
    [[nodiscard]] bool materialize_message(std::size_t message_len) noexcept;
    // 1.3 rewrites every sealed record's outer type to application_data;
    // 1.2 preserves the payload's true type under encryption (the AAD binds
    // it either way). Both fly at 0x0303.
    [[nodiscard]] TlsContentType outer_record_type(TlsContentType type) const noexcept {
        return version_ == TlsProtocolVersion::Tls13 ? TlsContentType::ApplicationData : type;
    }
    // Frames payload as sealed records into out_ (<=16 KiB per record; 1.3
    // outer application_data, 1.2 type-preserved, both at 0x0303; empty
    // payload → one zero-length record).
    [[nodiscard]] common::IoResult<void> emit_sealed(TlsContentType type,
                                                     std::span<const std::uint8_t> payload) noexcept;

    TlsRecordCipher read_cipher_;
    TlsRecordCipher write_cipher_;
    mem::IoBufChain out_{};
    mem::IoBufChain plaintext_{}; // delivered app data awaiting read()
    mem::IoBufChain reassembly_{}; // straddling post-handshake message bytes
    mem::IoBuf message_buf_{}; // materialized straddling message (owns the dispatched body)
    TlsSecret own_secret_; // 1.3 write-side rekey base; empty on 1.2
    TlsSecret peer_secret_; // 1.3 read-side rekey base; empty on 1.2
    std::array<std::uint8_t, 256> alpn_{};
    TlsProtocolVersion version_;
    TlsCipherSuiteId suite_;
    std::uint16_t alpn_len_ = 0;
    std::uint8_t warning_alerts_ = 0; // consecutive dropped warnings (see on_alert_bytes)
    bool rekey_pending_ = false; // peer's KeyUpdate(update_requested): owe a response before the next write
    bool peer_closed_ = false; // the peer's close_notify latched
    bool close_sent_ = false; // our close_notify encoded
    bool failed_ = false; // terminal latched (ours or the peer's)
};

} // namespace fiber::tls

#endif // FIBER_TLS_TLS_CONNECTION_H
