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
// schedule) lives behind a pimpl in src/tls — this header pulls no OpenSSL.

#include <cstddef>
#include <cstdint>
#include <span>

#include <fiber/common/IoError.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/common/mem/IoBuf.h>
#include <fiber/common/mem/IoBufChain.h>
#include <fiber/tls/TlsConfig.h>
#include <fiber/tls/TlsConnectedState.h>
#include <fiber/tls/TlsTypes.h>

namespace fiber::mem {
class IoBufNodePool;
}

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
    // The pool backs the engine's record chains; destroy the engine (and any
    // output it produced) on the loop owning that pool. Construction failure
    // (entropy / allocation) lands directly in the Failed terminal state.
    TlsClientHandshakeEngine(const TlsClientConfig &config, const TlsSessionOffer *session,
                             mem::IoBufNodePool &pool) noexcept;
    ~TlsClientHandshakeEngine();

    // Peer bytes in arbitrary chunking; the engine digests everything it can.
    // IoErr only NoMem (a connection-level failure — the glue tears down).
    // Feeding a terminal engine is a contract violation (FIBER_ASSERT): the
    // glue swaps to the next phase object at HandshakeDone/Failed.
    [[nodiscard]] common::IoResult<Event> feed(mem::IoBuf &&bytes) noexcept;
    [[nodiscard]] common::IoResult<Event> feed(mem::IoBufChain &&bytes) noexcept;

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
    // their sequence continuity). Requires done() && !failed().
    [[nodiscard]] TlsConnectedState take_state() noexcept;

    // HandshakeDone: inbound bytes the handshake never consumed (app data
    // piggybacked behind the final flight, or a post-handshake NST that
    // arrived in the same feed — the engine stops stepping at done): feed
    // them into the TlsConnection built from take_state(). A second take is
    // empty.
    [[nodiscard]] mem::IoBufChain take_inbound_leftover() noexcept;

private:
    struct Impl;
    Impl *impl_ = nullptr; // src-side state; nullptr only on ctor failure
};

} // namespace fiber::tls

#endif // FIBER_TLS_HANDSHAKE_TLS_CLIENT_HANDSHAKE_ENGINE_H
