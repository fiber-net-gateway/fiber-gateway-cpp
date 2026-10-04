#ifndef FIBER_TLS_HANDSHAKE_TLS_SERVER_HANDSHAKE_ENGINE_H
#define FIBER_TLS_HANDSHAKE_TLS_SERVER_HANDSHAKE_ENGINE_H

// TLS server handshake engine (feature/tls/07) — the mirror of the 06 client
// engine. A CH-driven FSM: construction produces no first flight; the first
// feed of a complete ClientHello selects the version (supported_versions
// carries 0x0304 → the 1.3 sub-flow; a 0x0303 legacy/offer fallback → the
// 1.2 sub-flow) and answers with the ServerHello flight. Everything record-
// mechanical lives in the shared TlsHandshakeContext; the engine owns the
// context, the retained ClientHello state, and one mounted version sub-flow
// by value (~72 KiB, flight scratch included — the glue holds the engine
// where that size is paid once per handshake, e.g. a coroutine frame).
//
// Config/resumption/minter are all borrowed and must outlive the engine (the
// net glue holds them). resumption/minter == nullptr: no resumption lookup,
// no NST minting (a client PSK offer then degrades safely to a full
// handshake). Chains resolve the current loop's node pool per operation —
// run and destroy the engine on the connection's loop.
//
// source (09 §4.1): optional per-ClientHello config selection. When set, the
// fork calls select() right after decoding the ClientHello; null return =
// the hello selects none → handshake_failure. The returned config's spans
// borrow CALLER-owned material that must outlive the engine (net glue
// staging), like every other input.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>

#include "../detail/TlsHandshakeContext.h"
#include "Tls12ServerHandshake.h"
#include "Tls13ServerHandshake.h"
#include "TlsServerHandshakeShared.h"

#include <fiber/common/IoError.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/common/mem/IoBuf.h>
#include <fiber/common/mem/IoBufChain.h>
#include <fiber/tls/TlsConfig.h>
#include <fiber/tls/TlsConnectedState.h>
#include <fiber/tls/TlsTypes.h>

namespace fiber::tls {

class TlsServerHandshakeEngine final : public common::NonCopyable, common::NonMovable {
public:
    enum class Event : std::uint8_t { None, HandshakeDone, Failed }; // as in 06

    TlsServerHandshakeEngine(const TlsServerConfig &config, const TlsResumptionLookup *resumption,
                             const TlsTicketMinter *minter, const TlsServerConfigSource *source = nullptr) noexcept;

    // Client bytes in (any chunking). NoMem = connection-level failure;
    // feeding a terminal engine is a FIBER_ASSERT.
    [[nodiscard]] common::IoResult<Event> feed(mem::IoBuf &&bytes) noexcept;
    [[nodiscard]] common::IoResult<Event> feed(mem::IoBufChain &&bytes) noexcept;

    // QUIC mode (10 §3.3): CRYPTO-stream bytes at `level` — raw handshake
    // messages, no record framing. The QUIC layer gates level ordering.
    // Requires a config with quic callbacks; mutually exclusive with feed().
    // Takes all nodes from bytes before pumping; bytes is empty even if pumping fails.
    // Input must not carry the stream-complete flag.
    [[nodiscard]] common::IoResult<Event> feed_quic(TlsQuicLevel level, mem::IoBufChain &bytes) noexcept;

    // 0-RTT read path: decrypted early-data plaintext (non-empty only when
    // accepted; bounded by 14336). The glue drains it before and after
    // HandshakeDone alike; a second take yields an empty chain.
    [[nodiscard]] mem::IoBufChain take_early_data() noexcept;

    [[nodiscard]] mem::IoBufChain take_output() noexcept;
    [[nodiscard]] bool done() const noexcept;
    [[nodiscard]] bool failed() const noexcept;
    [[nodiscard]] TlsAlertDesc failure_alert() const noexcept;
    // Requires done && !failed. QUIC mode: contract violation —
    // take_quic_result() is that mode's handoff.
    [[nodiscard]] TlsConnectedState take_state() noexcept;

    // HandshakeDone in QUIC mode (10 §8): the tail delivery. Requires
    // done() && !failed() && config.quic != nullptr.
    [[nodiscard]] TlsQuicHandshakeResult take_quic_result() noexcept;

    // Requires done && !failed. Inbound bytes the handshake never consumed
    // (app data piggybacked behind the final flight — the engine stops
    // stepping at HandshakeDone): feed them into the TlsConnection built
    // from take_state(). A second take is empty.
    [[nodiscard]] mem::IoBufChain take_inbound_leftover() noexcept;

private:
    static constexpr std::size_t kScratchCap = 32768; // server-flight staging (chains ≪ 32 KiB)

    void fail_local(TlsAlertDesc alert) noexcept;
    void fail_peer(TlsAlertDesc alert) noexcept;
    // Digests inbound bytes to quiescence. Pre-fork this includes the
    // version decision on the first handshake message; post-fork every
    // Message/CCS routes to the mounted sub-flow.
    [[nodiscard]] common::IoResult<Event> pump() noexcept;
    // The fork point (07 §4.1): retain + decode the ClientHello, decide the
    // version from its shape, mount the sub-flow, and hand the decoded view
    // over — every version-specific validation rule belongs to the sub-flow
    // that owns that version.
    void handle_first_message(TlsHandshakeType type, std::span<const std::uint8_t> body) noexcept;

    // The mounted sub-flow's destructor wipes its own secrets; the key
    // exchange wipes itself; secrets already moved into a taken
    // TlsConnectedState arrive moved-from (pre-wiped).

    // ---- inputs (borrowed; the net glue outlives the engine) ----
    TlsServerConfig cfg_;
    const TlsResumptionLookup *resumption_;
    const TlsTicketMinter *minter_;
    const TlsServerConfigSource *source_; // optional per-CH selection (09 §4.1)

    // ---- pipeline + pre-fork state ----
    TlsHandshakeContext ctx_;
    TlsServerHelloState hello_; // retained ClientHello + negotiation intermediates
    mem::IoBufChain early_; // decrypted 0-RTT plaintext (P5 fills; take_early_data drains)
    TlsServerHandshakeOutcome out_; // terminal channel both halves write
    // Staged flights (sub-exclusive post-fork). Not zero-initialized: every
    // encoder writes the full span it reports before anything reads it.
    std::array<std::uint8_t, kScratchCap> scratch_;

    // ---- version sub-flow, mounted at the ClientHello fork ----
    std::variant<std::monostate, Tls13ServerHandshake, Tls12ServerHandshake> flow_;
};

} // namespace fiber::tls

#endif // FIBER_TLS_HANDSHAKE_TLS_SERVER_HANDSHAKE_ENGINE_H
