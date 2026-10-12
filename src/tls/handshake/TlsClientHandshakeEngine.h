#ifndef FIBER_TLS_HANDSHAKE_TLS_CLIENT_HANDSHAKE_ENGINE_H
#define FIBER_TLS_HANDSHAKE_TLS_CLIENT_HANDSHAKE_ENGINE_H

// TLS client handshake engine (06 §3): the complete client-side handshake
// FSM. Constructing it produces the first flight (ClientHello ± PSK binder ±
// middlebox compat CCS) into the outbound buffer; feed() of peer bytes drives
// the state machine; Event::HandshakeDone hands a TlsConnectedState over,
// Event::Failed is terminal (the fatal alert, if any, is already encoded in
// the outbound buffer).
//
// Synchronous, no fds, no coroutines, no clock: every state advance is
// feed-driven. The engine is memory-only plumbing — the net glue (09) owns
// the socket loop: construct → take_output → write → feed(read) → repeat.
//
// The engine has no version starting point: the offered supported_versions
// carries [1.3, 1.2] and the ServerHello read point decides which sub-flow
// runs (06 §4.1). Internal state (record pipeline, transcripts, key
// schedule, flight scratch) is held by value — ~64 KiB, so the glue holds the
// engine where that size is paid once per handshake (e.g. a coroutine frame).

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>

#include "../detail/TlsHandshakeContext.h"
#include "Tls12ClientHandshake.h"
#include "Tls13ClientHandshake.h"
#include "TlsClientHandshakeShared.h"

#include <fiber/common/IoError.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/common/mem/IoBuf.h>
#include <fiber/common/mem/IoBufChain.h>
#include <fiber/tls/TlsConfig.h>
#include <fiber/tls/TlsConnectedState.h>
#include <fiber/tls/TlsTypes.h>

namespace fiber::tls {

class TlsClientHandshakeEngine final : public common::NonCopyable, public common::NonMovable {
public:
    enum class Event : std::uint8_t {
        None, // digested a partial flight / need more input; keep feeding
        HandshakeDone, // terminal (success): take_state() is ready; out may still hold tail bytes
        Failed, // terminal (failure): the alert, if sent, is in out; failure_alert() says why
    };

    // `config` and `session` are BORROWED views that must outlive the engine
    // (the net glue holds them). `session` == nullptr runs a full handshake.
    // Chains resolve the current loop's node pool per operation — run and
    // destroy the engine on the connection's loop. Construction failure
    // (entropy / allocation) lands directly in the Failed terminal state.
    TlsClientHandshakeEngine(const TlsClientConfig &config, const TlsSessionOffer *session) noexcept;

    // Peer bytes in arbitrary chunking; the engine digests everything it can.
    // IoErr only NoMem (a connection-level failure — the glue tears down).
    // Feeding a terminal engine is a contract violation (FIBER_ASSERT): the
    // glue swaps to the next phase object at HandshakeDone/Failed.
    [[nodiscard]] common::IoResult<Event> feed(mem::IoBuf &&bytes) noexcept;
    [[nodiscard]] common::IoResult<Event> feed(mem::IoBufChain &&bytes) noexcept;

    // QUIC mode (10 §3.3): CRYPTO-stream bytes at `level` — raw handshake
    // messages, no record framing. The QUIC layer gates level ordering
    // (fully consuming each level's stream before advancing). Requires a
    // config with quic callbacks; mutually exclusive with feed().
    // Takes all nodes from bytes before pumping; bytes is empty even if pumping fails.
    // Input must not carry the stream-complete flag.
    [[nodiscard]] common::IoResult<Event> feed_quic(TlsQuicLevel level, mem::IoBufChain &bytes) noexcept;

    // 0-RTT write path. Available only while early data is offered
    // (session->max_early_data > 0) and not yet accepted or rejected; the
    // cumulative cap is the advertised max_early_data. MessageTooLarge when
    // `data` would exceed it (nothing written, earlier bytes kept); Invalid
    // once the window closed (rejected — or accepted and Done). Sealed with
    // the early-traffic cipher, already framed into the outbound buffer.
    [[nodiscard]] common::IoResult<void> write_early_data(std::span<const std::uint8_t> data) noexcept;

    // Hands the outbound byte stream over (a sequence of TLS records,
    // including any fatal alert). A second take yields an empty chain.
    [[nodiscard]] mem::IoBufChain take_output() noexcept;

    [[nodiscard]] bool done() const noexcept; // HandshakeDone or Failed
    [[nodiscard]] bool failed() const noexcept;
    // Failed: the alert that ended the handshake — ours (sent) or the peer's
    // (received). Requires done().
    [[nodiscard]] TlsAlertDesc failure_alert() const noexcept;
    // HandshakeDone: the connected-phase state (record ciphers moved in with
    // their sequence continuity). Requires done() && !failed(). QUIC mode:
    // contract violation — take_quic_result() is that mode's handoff.
    [[nodiscard]] TlsConnectedState take_state() noexcept;

    // HandshakeDone in QUIC mode (10 §8): the tail delivery — secrets went
    // out via set_secret during the handshake; this carries the resumption
    // master, ALPN, and the resume/0-RTT verdicts. Requires done() &&
    // !failed() && config.quic != nullptr.
    [[nodiscard]] TlsQuicHandshakeResult take_quic_result() noexcept;

    // HandshakeDone: inbound bytes the handshake never consumed (app data
    // piggybacked behind the final flight, or a post-handshake NST that
    // arrived in the same feed — the engine stops stepping at done): feed
    // them into the TlsConnection built from take_state(). A second take is
    // empty.
    [[nodiscard]] mem::IoBufChain take_inbound_leftover() noexcept;

private:
    static constexpr std::size_t kScratchCap = 32768; // client-flight staging (mTLS chains ≪ 32 KiB)

    void fail_local(TlsAlertDesc alert) noexcept;
    void fail_peer(TlsAlertDesc alert) noexcept;
    // Digests inbound bytes to quiescence. Pre-fork this includes the
    // version decision on the first handshake message; post-fork every
    // Message/CCS routes to the mounted sub-flow.
    [[nodiscard]] common::IoResult<Event> pump() noexcept;
    // The fork point (06 §4.2): decode the first handshake message, decide
    // the version from its shape, mount the sub-flow, and hand the RAW
    // message over — every ServerHello validation rule belongs to the
    // sub-flow that owns that version.
    void handle_first_message(TlsHandshakeType type, std::span<const std::uint8_t> body) noexcept;

    // The schedule and key exchange wipe themselves; the mounted sub-flow's
    // destructor wipes its own secrets; secrets already moved into a taken
    // TlsConnectedState arrive moved-from (pre-wiped).

    // ---- inputs (borrowed; the net glue outlives the engine) ----
    TlsClientConfig cfg_;
    const TlsSessionOffer *session_;

    // ---- pipeline + pre-fork flight state ----
    TlsHandshakeContext ctx_;
    TlsClientHelloState hello_; // retained ClientHello + its key exchange
    std::optional<TlsKeySchedule13> sched_; // PSK binder tree (pre-fork; 1.3 continues it)
    TlsClientEarlyWindow early_; // 0-RTT write window (outer API surface)
    TlsClientHandshakeOutcome out_; // terminal channel both halves write
    bool psk_offered_ = false;
    bool ccs_sent_ = false; // the one compat CCS went out (RFC 8446 D.4)
    // Staged flights (sub-exclusive post-fork). Not zero-initialized: every
    // encoder writes the full span it reports before anything reads it.
    std::array<std::uint8_t, kScratchCap> scratch_;

    // ---- version sub-flow, mounted at the ServerHello read point ----
    std::variant<std::monostate, Tls13ClientHandshake, Tls12ClientHandshake> flow_;
};

} // namespace fiber::tls

#endif // FIBER_TLS_HANDSHAKE_TLS_CLIENT_HANDSHAKE_ENGINE_H
