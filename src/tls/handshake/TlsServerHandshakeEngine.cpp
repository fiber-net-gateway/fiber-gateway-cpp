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
         const TlsTicketMinter *ticket_minter) noexcept :
        cfg(config), resumption(resumption_lookup), minter(ticket_minter) {}

    // The mounted sub-flow's destructor wipes its own secrets; the key
    // exchange wipes itself; secrets already moved into a taken
    // TlsConnectedState arrive moved-from (pre-wiped).

    // ---- inputs (borrowed; the net glue outlives the engine) ----
    TlsServerConfig cfg;
    const TlsResumptionLookup *resumption;
    const TlsTicketMinter *minter;

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

TlsServerHandshakeEngine::TlsServerHandshakeEngine(const TlsServerConfig &config, const TlsResumptionLookup *resumption,
                                                   const TlsTicketMinter *minter, mem::IoBufNodePool &pool) noexcept {
    impl_ = new (std::nothrow) Impl(config, resumption, minter);
    if (impl_ == nullptr) {
        return; // done()/failed() report the terminal state; no alert bytes
    }
    Impl &impl = *impl_;
    impl.ctx.bind(pool);
    impl.early = mem::IoBufChain(pool); // 0-RTT sink target (pool-bound; take_early_data drains)
    // Server plaintext records are 0x0303 from the very first one (the
    // client's 0x0301 first-flight convention is client-only, 06 §2.3).
    impl.ctx.set_legacy_version(kTlsRecordVersionTls12);

    // Configuration invariants at the boundary (no PSK-only mode exists).
    if (impl.cfg.chain == nullptr || impl.cfg.key == nullptr || impl.cfg.chain->empty() || impl.cfg.key->empty()) {
        impl.fail_local(TlsAlertDesc::InternalError); // configuration bug, not a protocol event
        return;
    }
    if (impl.cfg.require_client_cert && impl.cfg.client_trust == nullptr) {
        impl.fail_local(TlsAlertDesc::InternalError);
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

    // Null compression only (RFC 8446 §4.1.2 / RFC 5246 §7.4.1.4); the alert
    // is version-shaped.
    const bool null_compression = ch.compression_methods.size() == 1 && ch.compression_methods[0] == 0;

    // ---- version decision (07 §4.1) ----
    // supported_versions carries the offer list when present; a client
    // without it speaks its legacy_version. 1.3 first, then the 0x0303
    // fallback, else protocol_version.
    const bool offers13 =
            ch.has_supported_versions && tls_server_list_contains(ch.supported_versions, kTlsVersionTls13);
    const bool offers12 = ch.has_supported_versions ? tls_server_list_contains(ch.supported_versions, kTlsVersionTls12)
                                                    : ch.legacy_version >= kTlsVersionTls12;

    if (offers13) {
        if (!null_compression) {
            fail_local(TlsAlertDesc::IllegalParameter);
            return;
        }
        auto &sub = flow.emplace<Tls13ServerHandshake>(Tls13ServerHandshake::Mount{
                ctx, cfg, resumption, minter, hello, early, out, {scratch.data(), scratch.size()}});
        sub.start(ch, {hello.ch.data(), hello.ch_len});
        return;
    }
    if (offers12) {
        if (!null_compression) {
            fail_local(TlsAlertDesc::HandshakeFailure); // RFC 5246 §7.4.1.4
            return;
        }
        auto &sub = flow.emplace<Tls12ServerHandshake>(Tls12ServerHandshake::Mount{
                ctx, cfg, resumption, minter, hello, out, {scratch.data(), scratch.size()}});
        sub.start(ch, {hello.ch.data(), hello.ch_len});
        return;
    }
    fail_local(TlsAlertDesc::ProtocolVersion);
}

} // namespace fiber::tls
