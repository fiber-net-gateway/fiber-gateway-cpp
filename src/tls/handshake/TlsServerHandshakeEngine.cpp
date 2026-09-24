#include <fiber/tls/handshake/TlsServerHandshakeEngine.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>

#include "../detail/TlsHandshakeContext.h"
#include "Tls12ServerHandshake.h"
#include "Tls13ServerHandshake.h"
#include "TlsServerHandshakeShared.h"

#include <fiber/tls/TlsVersion.h>
#include <fiber/tls/handshake/TlsExtensionCodec.h>
#include <fiber/tls/handshake/TlsHandshakeCodec.h>
#include <fiber/tls/handshake/TlsHandshakeMessage.h>
#include <fiber/tls/record/TlsRecord.h>

namespace fiber::tls {

// =====================================================================
// Impl — the outer shell
// =====================================================================

// The version-neutral half of the engine (07 §4.1) — the mirror of the 06
// client shell with the direction flipped: construction produces NO first
// flight; the first complete ClientHello is the fork point where the version
// is decided and one sub-flow mounts (Tls13ServerHandshake or
// Tls12ServerHandshake). From that fork on, every inbound Message/CCS
// routes to the mounted sub-flow; the sub writes terminal status back
// through `out`.
struct TlsServerHandshakeEngine::Impl {
    static constexpr std::size_t kScratchCap = 32768; // server-flight staging (chains ≪ 32 KiB)

    Impl(const TlsServerConfig &config, const TlsResumptionLookup *resumption_lookup,
         const TlsTicketMinter *ticket_minter, const TlsServerConfigSource *config_source) noexcept :
        cfg(config), resumption(resumption_lookup), minter(ticket_minter), source(config_source) {}

    // The mounted sub-flow's destructor wipes its own secrets; the key
    // exchange wipes itself; secrets already moved into a taken
    // TlsConnectedState arrive moved-from (pre-wiped).

    // ---- inputs (borrowed; the net glue outlives the engine) ----
    TlsServerConfig cfg;
    const TlsResumptionLookup *resumption;
    const TlsTicketMinter *minter;
    const TlsServerConfigSource *source; // optional per-CH selection (09 §4.1)

    // ---- pipeline + pre-fork state ----
    TlsHandshakeContext ctx;
    TlsServerHelloState hello; // retained ClientHello + negotiation intermediates
    mem::IoBufChain early; // decrypted 0-RTT plaintext (P5 fills; take_early_data drains)
    TlsServerHandshakeOutcome out; // terminal channel both halves write
    std::array<std::uint8_t, kScratchCap> scratch{}; // staged flights (sub-exclusive post-fork)

    // ---- version sub-flow, mounted at the ClientHello fork ----
    std::variant<std::monostate, Tls13ServerHandshake, Tls12ServerHandshake> flow;

    void fail_local(TlsAlertDesc alert) noexcept {
        if (out.done) {
            return;
        }
        ctx.fail(alert); // encodes the fatal alert (sealed when the write cipher is live)
        out.failed = true;
        out.done = true;
        out.alert = alert;
    }

    void fail_peer(TlsAlertDesc alert) noexcept {
        if (out.done) {
            return;
        }
        out.failed = true;
        out.done = true;
        out.alert = alert; // the peer's alert; nothing is sent back
    }

    // Digests inbound bytes to quiescence. Pre-fork this includes the
    // version decision on the first handshake message; post-fork every
    // Message/CCS routes to the mounted sub-flow.
    [[nodiscard]] common::IoResult<TlsServerHandshakeEngine::Event> pump() noexcept {
        for (;;) {
            if (out.done) {
                return out.failed ? TlsServerHandshakeEngine::Event::Failed
                                  : TlsServerHandshakeEngine::Event::HandshakeDone;
            }
            const TlsInboundStep step = ctx.step();
            switch (step.kind) {
                case TlsInboundStep::Kind::NeedMore:
                    return TlsServerHandshakeEngine::Event::None;
                case TlsInboundStep::Kind::Fatal:
                    fail_local(step.alert);
                    return TlsServerHandshakeEngine::Event::Failed;
                case TlsInboundStep::Kind::Alert:
                    // Mid-handshake every inbound alert is terminal (06 §2.3
                    // semantics, mirrored) — close_notify included. Nothing is
                    // sent back to a connection that already told us it is dying.
                    fail_peer(step.alert);
                    return TlsServerHandshakeEngine::Event::Failed;
                case TlsInboundStep::Kind::Ccs:
                    // A well-formed client CCS is middlebox compat noise the
                    // 1.3 sub-flow ignores (the context validated the 1-byte
                    // 0x01 form); the 1.2 sub-flow swaps the read side at its
                    // expected point.
                    if (auto *sub = std::get_if<Tls13ServerHandshake>(&flow)) {
                        sub->on_ccs();
                    } else if (auto *sub = std::get_if<Tls12ServerHandshake>(&flow)) {
                        sub->on_ccs();
                    }
                    break;
                case TlsInboundStep::Kind::Message:
                    if (auto *sub = std::get_if<Tls13ServerHandshake>(&flow)) {
                        sub->on_message(step.type, step.body);
                    } else if (auto *sub = std::get_if<Tls12ServerHandshake>(&flow)) {
                        sub->on_message(step.type, step.body);
                    } else {
                        handle_first_message(step.type, step.body);
                    }
                    break;
            }
        }
    }

    // The fork point (07 §4.1): retain + decode the ClientHello, decide the
    // version from its shape, mount the sub-flow, and hand the decoded view
    // over — every version-specific validation rule belongs to the sub-flow
    // that owns that version.
    void handle_first_message(TlsHandshakeType type, std::span<const std::uint8_t> body) noexcept;
};

// =====================================================================
// Construction (no first flight — the CH drives everything)
// =====================================================================

namespace {

// The ctor invariants, re-checked verbatim after a selector swap (the
// selected config is as much caller input as the ctor one). Version bounds
// mirror the client engine's check (09 §4.2): domain {1.2, 1.3}, non-empty
// ordered window.
bool config_versions_hold(const TlsServerConfig &cfg) noexcept {
    if (cfg.require_client_cert && cfg.client_trust == nullptr) {
        return false;
    }
    return cfg.min_version <= cfg.max_version && cfg.min_version >= kTlsVersionTls12 &&
           cfg.max_version <= kTlsVersionTls13;
}
bool config_invariants_hold(const TlsServerConfig &cfg) noexcept {
    if (cfg.chain == nullptr || cfg.key == nullptr || cfg.chain->empty() || cfg.key->empty()) {
        return false; // no PSK-only mode exists
    }
    return config_versions_hold(cfg);
}

} // namespace

TlsServerHandshakeEngine::TlsServerHandshakeEngine(const TlsServerConfig &config, const TlsResumptionLookup *resumption,
                                                   const TlsTicketMinter *minter,
                                                   const TlsServerConfigSource *source) noexcept {
    impl_ = new (std::nothrow) Impl(config, resumption, minter, source);
    if (impl_ == nullptr) {
        return; // done()/failed() report the terminal state; no alert bytes
    }
    Impl &impl = *impl_;
    // Server plaintext records are 0x0303 from the very first one (the
    // client's 0x0301 first-flight convention is client-only, 06 §2.3).
    impl.ctx.set_legacy_version(kTlsRecordVersionTls12);

    if (impl.cfg.quic != nullptr) {
        // QUIC (10 §3): records never frame; the HRR (if any) rides Initial
        // CRYPTO, the SH flight Handshake. The context binds the template's
        // callbacks here — a per-ClientHello selector may not change them
        // (asserted at the fork).
        FIBER_ASSERT(impl.cfg.min_version == kTlsVersionTls13 && impl.cfg.max_version == kTlsVersionTls13);
        impl.ctx.enable_quic(*impl.cfg.quic);
    }

    // Configuration invariants at the boundary (no PSK-only mode exists).
    // With a per-ClientHello selector the ctor config is a TEMPLATE:
    // chain/key arrive with the selection (checked at the fork); only the
    // version window and the mTLS coupling must already hold.
    const bool selected_per_hello = source != nullptr && source->select != nullptr;
    if (!((selected_per_hello && config_versions_hold(impl.cfg)) || config_invariants_hold(impl.cfg))) {
        impl.fail_local(TlsAlertDesc::InternalError); // configuration bug, not a protocol event
        return;
    }
}

TlsServerHandshakeEngine::~TlsServerHandshakeEngine() { delete impl_; }

// =====================================================================
// feed / early data / output / state
// =====================================================================

common::IoResult<TlsServerHandshakeEngine::Event> TlsServerHandshakeEngine::feed(mem::IoBuf &&bytes) noexcept {
    FIBER_ASSERT(impl_ != nullptr && !impl_->out.done);
    if (!impl_->ctx.feed(std::move(bytes))) {
        return std::unexpected(common::IoErr::NoMem);
    }
    return impl_->pump();
}

common::IoResult<TlsServerHandshakeEngine::Event> TlsServerHandshakeEngine::feed(mem::IoBufChain &&bytes) noexcept {
    FIBER_ASSERT(impl_ != nullptr && !impl_->out.done);
    if (!impl_->ctx.feed(std::move(bytes))) {
        return std::unexpected(common::IoErr::NoMem);
    }
    return impl_->pump();
}

common::IoResult<TlsServerHandshakeEngine::Event>
TlsServerHandshakeEngine::feed_quic(TlsQuicLevel level, std::span<const std::uint8_t> bytes) noexcept {
    FIBER_ASSERT(impl_ != nullptr && !impl_->out.done);
    if (!impl_->ctx.provide_quic(level, bytes)) {
        return std::unexpected(common::IoErr::NoMem);
    }
    return impl_->pump();
}

mem::IoBufChain TlsServerHandshakeEngine::take_early_data() noexcept {
    if (impl_ == nullptr) {
        return mem::IoBufChain{};
    }
    return std::move(impl_->early);
}

mem::IoBufChain TlsServerHandshakeEngine::take_output() noexcept {
    if (impl_ == nullptr) {
        return mem::IoBufChain{};
    }
    return impl_->ctx.take_output();
}

bool TlsServerHandshakeEngine::done() const noexcept { return impl_ == nullptr || impl_->out.done; }

bool TlsServerHandshakeEngine::failed() const noexcept { return impl_ == nullptr || impl_->out.failed; }

TlsAlertDesc TlsServerHandshakeEngine::failure_alert() const noexcept {
    FIBER_ASSERT(done());
    return impl_ != nullptr ? impl_->out.alert : TlsAlertDesc::InternalError;
}

TlsConnectedState TlsServerHandshakeEngine::take_state() noexcept {
    FIBER_ASSERT(impl_ != nullptr && impl_->out.done && !impl_->out.failed);
    FIBER_ASSERT(impl_->cfg.quic == nullptr); // QUIC hands over via take_quic_result (10 定谳 2)
    return std::visit(
            [](auto &sub) -> TlsConnectedState {
                using Sub = std::decay_t<decltype(sub)>;
                if constexpr (std::is_same_v<Sub, std::monostate>) {
                    FIBER_ASSERT(false); // done && !failed implies a sub-flow finished
                    return TlsConnectedState{};
                } else {
                    return sub.take_state();
                }
            },
            impl_->flow);
}

TlsQuicHandshakeResult TlsServerHandshakeEngine::take_quic_result() noexcept {
    FIBER_ASSERT(impl_ != nullptr && impl_->out.done && !impl_->out.failed);
    FIBER_ASSERT(impl_->cfg.quic != nullptr); // the QUIC-mode twin of take_state
    return std::visit(
            [](auto &sub) -> TlsQuicHandshakeResult {
                using Sub = std::decay_t<decltype(sub)>;
                if constexpr (std::is_same_v<Sub, Tls13ServerHandshake>) {
                    return sub.take_quic_result();
                } else {
                    FIBER_ASSERT(false); // QUIC never mounts monostate-done or the 1.2 sub-flow
                    return TlsQuicHandshakeResult{};
                }
            },
            impl_->flow);
}

mem::IoBufChain TlsServerHandshakeEngine::take_inbound_leftover() noexcept {
    FIBER_ASSERT(impl_ != nullptr && impl_->out.done && !impl_->out.failed);
    return impl_->ctx.take_inbound_leftover();
}

// =====================================================================
// The fork: ClientHello version detection
// =====================================================================

void TlsServerHandshakeEngine::Impl::handle_first_message(TlsHandshakeType type,
                                                          std::span<const std::uint8_t> body) noexcept {
    if (type != TlsHandshakeType::ClientHello) {
        fail_local(TlsAlertDesc::UnexpectedMessage);
        return;
    }
    // Retain the body BEFORE decoding — the decoded view's spans must
    // survive past this step()'s borrow (the mounted sub-flow keeps using
    // them through the whole handshake).
    if (body.size() > TlsServerHelloState::kCap) {
        fail_local(TlsAlertDesc::HandshakeFailure); // retained-copy policy bound (16 KiB ≫ real CHs)
        return;
    }
    std::memcpy(hello.ch.data(), body.data(), body.size());
    hello.ch_len = body.size();
    if (!tls_decode_client_hello(hello.ch.data(), hello.ch_len, hello.view).has_value()) {
        fail_local(TlsAlertDesc::DecodeError);
        return;
    }
    const TlsClientHello &ch = hello.view;

    // Per-ClientHello config selection (09 §4.1): the selector runs right
    // after the decode, before any version/policy decision — the returned
    // config drives THIS connection. The spans it hands back borrow caller
    // staging that outlives the engine; null = the hello selects none (an
    // SNI this host does not serve) → handshake_failure.
    if (source != nullptr && source->select != nullptr) {
        const TlsServerConfig *selected = source->select(source->ctx, ch);
        if (selected == nullptr || !config_invariants_hold(*selected)) {
            fail_local(TlsAlertDesc::HandshakeFailure);
            return;
        }
        FIBER_ASSERT(selected->quic == cfg.quic); // the context is QUIC-bound to the template's callbacks
        cfg = *selected;
    }

    if (cfg.quic != nullptr) {
        // Middlebox compatibility mode is prohibited in QUIC (RFC 9001 §8.4):
        // a non-empty legacy_session_id can only be a compat-mode client.
        // BoringSSL answers illegal_parameter (reason
        // UNEXPECTED_COMPATIBILITY_MODE).
        if (!ch.session_id.empty()) {
            fail_local(TlsAlertDesc::IllegalParameter);
            return;
        }
        // 0x39 extraction (10 §5): the CH's transport parameters, handed to
        // the QUIC layer the moment they decode (the span borrows the
        // retained copy — the callback copies). Absence is the QUIC layer's
        // completeness check to raise.
        std::span<const std::uint8_t> params{};
        if (tls_find_extension_payload(ch.extensions_block, TlsExtensionType::QuicTransportParameters, params)) {
            cfg.quic->on_peer_transport_params(cfg.quic->ctx, params);
        }
    }

    // Null compression only (RFC 8446 §4.1.2 / RFC 5246 §7.4.1.4); the alert
    // is version-shaped.
    const bool null_compression = ch.compression_methods.size() == 1 && ch.compression_methods[0] == 0;

    // ---- version decision (07 §4.1, bounds added in 09 §4.2) ----
    // supported_versions carries the offer list when present; a client
    // without it speaks its legacy_version. The offer must intersect the
    // config window [min, max]; with the default {1.2, 1.3} window this is
    // bit-identical to the 07 decision. 1.3 first, then the 0x0303
    // fallback, else protocol_version.
    const bool offers13 =
            ch.has_supported_versions && tls_server_list_contains(ch.supported_versions, kTlsVersionTls13);
    const bool offers12 = ch.has_supported_versions ? tls_server_list_contains(ch.supported_versions, kTlsVersionTls12)
                                                    : ch.legacy_version >= kTlsVersionTls12;
    const bool allow13 = offers13 && cfg.max_version >= kTlsVersionTls13;
    const bool allow12 = offers12 && cfg.min_version <= kTlsVersionTls12 && cfg.max_version >= kTlsVersionTls12 &&
                         cfg.quic == nullptr; // QUIC is 1.3-only (10 §3.4) — a 1.2 fork cannot mount

    if (allow13) {
        if (!null_compression) {
            fail_local(TlsAlertDesc::IllegalParameter);
            return;
        }
        auto &sub = flow.emplace<Tls13ServerHandshake>(Tls13ServerHandshake::Mount{
                ctx, cfg, resumption, minter, hello, early, out, {scratch.data(), scratch.size()}});
        sub.start(ch, {hello.ch.data(), hello.ch_len});
        return;
    }
    if (allow12) {
        if (!null_compression) {
            fail_local(TlsAlertDesc::HandshakeFailure); // RFC 5246 §7.4.1.4
            return;
        }
        FIBER_ASSERT(cfg.quic == nullptr); // the allow12 gate above is QUIC-blind by construction
        auto &sub = flow.emplace<Tls12ServerHandshake>(Tls12ServerHandshake::Mount{
                ctx, cfg, resumption, minter, hello, out, {scratch.data(), scratch.size()}});
        sub.start(ch, {hello.ch.data(), hello.ch_len});
        return;
    }
    fail_local(TlsAlertDesc::ProtocolVersion);
}

} // namespace fiber::tls
