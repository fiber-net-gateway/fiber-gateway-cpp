#ifndef FIBER_TLS_HANDSHAKE_TLS_SERVER_HANDSHAKE_ENGINE_H
#define FIBER_TLS_HANDSHAKE_TLS_SERVER_HANDSHAKE_ENGINE_H

// TLS server handshake engine (feature/tls/07) — the mirror of the 06 client
// engine. A CH-driven FSM: construction produces no first flight; the first
// feed of a complete ClientHello selects the version (supported_versions
// carries 0x0304 → the 1.3 sub-flow; a 0x0303 legacy/offer fallback → the
// 1.2 sub-flow) and answers with the ServerHello flight. Everything record-
// mechanical lives in the shared TlsHandshakeContext; this class is the
// public shell over an Impl that owns the context, the retained ClientHello
// state, and one mounted version sub-flow.
//
// Config/resumption/minter are all borrowed and must outlive the engine (the
// net glue holds them). resumption/minter == nullptr: no resumption lookup,
// no NST minting (a client PSK offer then degrades safely to a full
// handshake). Pool semantics as in 06: every chain binds to the caller's
// IoBufNodePool and is destroyed on the owning loop.
//
// source (09 §4.1): optional per-ClientHello config selection. When set, the
// fork calls select() right after decoding the ClientHello; null return =
// the hello selects none → handshake_failure. The returned config's spans
// borrow CALLER-owned material that must outlive the engine (net glue
// staging), like every other input.

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../common/IoError.h"
#include "../../common/NonCopyable.h"
#include "../../common/NonMovable.h"
#include "../../common/mem/IoBuf.h"
#include "../../common/mem/IoBufChain.h"
#include "../TlsConfig.h"
#include "../TlsConnectedState.h"
#include "../TlsTypes.h"

namespace fiber::tls {

class TlsServerHandshakeEngine final : public common::NonCopyable, common::NonMovable {
public:
    enum class Event : std::uint8_t { None, HandshakeDone, Failed }; // as in 06

    TlsServerHandshakeEngine(const TlsServerConfig &config, const TlsResumptionLookup *resumption,
                             const TlsTicketMinter *minter, mem::IoBufNodePool &pool,
                             const TlsServerConfigSource *source = nullptr) noexcept;
    ~TlsServerHandshakeEngine();

    // Client bytes in (any chunking). NoMem = connection-level failure;
    // feeding a terminal engine is a FIBER_ASSERT.
    [[nodiscard]] common::IoResult<Event> feed(mem::IoBuf &&bytes) noexcept;
    [[nodiscard]] common::IoResult<Event> feed(mem::IoBufChain &&bytes) noexcept;

    // 0-RTT read path: decrypted early-data plaintext (non-empty only when
    // accepted; bounded by 14336). The glue drains it before and after
    // HandshakeDone alike; a second take yields an empty chain.
    [[nodiscard]] mem::IoBufChain take_early_data() noexcept;

    [[nodiscard]] mem::IoBufChain take_output() noexcept;
    [[nodiscard]] bool done() const noexcept;
    [[nodiscard]] bool failed() const noexcept;
    [[nodiscard]] TlsAlertDesc failure_alert() const noexcept;
    // Requires done && !failed.
    [[nodiscard]] TlsConnectedState take_state() noexcept;

    // Requires done && !failed. Inbound bytes the handshake never consumed
    // (app data piggybacked behind the final flight — the engine stops
    // stepping at HandshakeDone): feed them into the TlsConnection built
    // from take_state(). A second take is empty.
    [[nodiscard]] mem::IoBufChain take_inbound_leftover() noexcept;

private:
    struct Impl;
    Impl *impl_ = nullptr; // null only on allocation failure (done()/failed() report it)
};

} // namespace fiber::tls

#endif // FIBER_TLS_HANDSHAKE_TLS_SERVER_HANDSHAKE_ENGINE_H
