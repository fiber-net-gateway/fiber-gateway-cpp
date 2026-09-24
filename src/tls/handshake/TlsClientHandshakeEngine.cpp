#include <fiber/tls/handshake/TlsClientHandshakeEngine.h>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>

#include "../detail/TlsHandshakeContext.h"
#include "Tls12ClientHandshake.h"
#include "Tls13ClientHandshake.h"
#include "TlsClientHandshakeShared.h"

#include <fiber/tls/TlsVersion.h>
#include <fiber/tls/handshake/TlsHandshakeCodec.h>
#include <fiber/tls/handshake/TlsHandshakeMessage.h>
#include <fiber/tls/record/TlsRecord.h>

namespace fiber::tls {

// =====================================================================
// Impl — the outer shell
// =====================================================================

// The version-neutral half of the engine (06 §4.1): the first flight
// (ClientHello ± PSK binder ± compat CCS ± the 0-RTT window), the record
// pipeline, and the ServerHello read point where the version forks. From
// that fork on, `flow` holds exactly one sub-flow — Tls13ClientHandshake or
// Tls12ClientHandshake — and every inbound Message/CCS routes to it; the
// sub writes terminal status back through `out`.
struct TlsClientHandshakeEngine::Impl {
    static constexpr std::size_t kScratchCap = 32768; // client-flight staging (mTLS chains ≪ 32 KiB)

    Impl(const TlsClientConfig &config, const TlsSessionOffer *session_offer) noexcept :
        cfg(config), session(session_offer) {}

    // The schedule and key exchange wipe themselves; the mounted sub-flow's
    // destructor wipes its own secrets; secrets already moved into a taken
    // TlsConnectedState arrive moved-from (pre-wiped).

    // ---- inputs (borrowed; the net glue outlives the engine) ----
    TlsClientConfig cfg;
    const TlsSessionOffer *session;

    // ---- pipeline + pre-fork flight state ----
    TlsHandshakeContext ctx;
    TlsClientHelloState hello; // retained ClientHello + its key exchange
    std::optional<TlsKeySchedule13> sched; // PSK binder tree (pre-fork; 1.3 continues it)
    TlsClientEarlyWindow early; // 0-RTT write window (outer API surface)
    TlsClientHandshakeOutcome out; // terminal channel both halves write
    bool psk_offered = false;
    bool ccs_sent = false; // the one compat CCS went out (RFC 8446 D.4)
    std::array<std::uint8_t, kScratchCap> scratch{}; // staged flights (sub-exclusive post-fork)

    // ---- version sub-flow, mounted at the ServerHello read point ----
    std::variant<std::monostate, Tls13ClientHandshake, Tls12ClientHandshake> flow;

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
    [[nodiscard]] common::IoResult<TlsClientHandshakeEngine::Event> pump() noexcept {
        for (;;) {
            if (out.done) {
                return out.failed ? TlsClientHandshakeEngine::Event::Failed
                                  : TlsClientHandshakeEngine::Event::HandshakeDone;
            }
            const TlsInboundStep step = ctx.step();
            switch (step.kind) {
                case TlsInboundStep::Kind::NeedMore:
                    return TlsClientHandshakeEngine::Event::None;
                case TlsInboundStep::Kind::Fatal:
                    fail_local(step.alert);
                    return TlsClientHandshakeEngine::Event::Failed;
                case TlsInboundStep::Kind::Alert:
                    // Mid-handshake every inbound alert is terminal (06 §2.3) —
                    // close_notify included: the peer walked away mid-flight. Nothing
                    // is sent back to a connection that already told us it is dying.
                    fail_peer(step.alert);
                    return TlsClientHandshakeEngine::Event::Failed;
                case TlsInboundStep::Kind::Ccs:
                    // Pre-fork and 1.3: a well-formed peer CCS is middlebox
                    // compat noise the client ignores (the context validated
                    // the 1-byte 0x01 form). Only the 1.2 sub-flow reacts.
                    if (auto *sub = std::get_if<Tls12ClientHandshake>(&flow)) {
                        sub->on_ccs();
                    }
                    break;
                case TlsInboundStep::Kind::Message:
                    if (auto *sub13 = std::get_if<Tls13ClientHandshake>(&flow)) {
                        sub13->on_message(step.type, step.body);
                    } else if (auto *sub12 = std::get_if<Tls12ClientHandshake>(&flow)) {
                        sub12->on_message(step.type, step.body);
                    } else {
                        handle_first_message(step.type, step.body);
                    }
                    break;
            }
        }
    }

    // The fork point (06 §4.2): decode the first handshake message, decide
    // the version from its shape, mount the sub-flow, and hand the RAW
    // message over — every ServerHello validation rule belongs to the
    // sub-flow that owns that version.
    void handle_first_message(TlsHandshakeType type, std::span<const std::uint8_t> body) noexcept;
};

// =====================================================================
// ClientHello first flight
// =====================================================================

TlsClientHandshakeEngine::TlsClientHandshakeEngine(const TlsClientConfig &config,
                                                   const TlsSessionOffer *session) noexcept {
    impl_ = new (std::nothrow) Impl(config, session);
    if (impl_ == nullptr) {
        return; // done()/failed() report the terminal state; no alert bytes
    }
    Impl &impl = *impl_;
    impl.ctx.set_legacy_version(kTlsRecordVersionTls10); // first flight (06 §2.3)

    if (impl.cfg.quic != nullptr) {
        // QUIC (10 §3): records never frame; the CH rides Initial-level
        // CRYPTO. The glue pins the version window to 1.3 (QUIC is 1.3-only)
        // — asserted here, enforced at the fork by the 09 version gates.
        FIBER_ASSERT(impl.cfg.min_version == kTlsVersionTls13 && impl.cfg.max_version == kTlsVersionTls13);
        impl.ctx.enable_quic(*impl.cfg.quic);
        // ALPN is mandatory in QUIC (RFC 9001 §8.1) — BoringSSL refuses the
        // CH build with an empty offer list; answer the config bug up front.
        if (impl.cfg.alpn.empty()) {
            impl.fail_local(TlsAlertDesc::InternalError);
            return;
        }
    }

    impl.psk_offered = session != nullptr;
    impl.early.offered_ext = impl.psk_offered && session->max_early_data > 0;

    if (impl.cfg.verify_peer && impl.cfg.trust == nullptr) {
        impl.fail_local(TlsAlertDesc::InternalError); // configuration bug, not a protocol event
        return;
    }
    // Version bounds invariant (09 §4.2): the domain is {1.2, 1.3} (1.3 is
    // the implementation ceiling) and the window must be non-empty. Like the
    // trust check above, this is a configuration bug — answer it up front,
    // not as a mystery failure after the first flight.
    if (impl.cfg.min_version > impl.cfg.max_version || impl.cfg.min_version < kTlsVersionTls12 ||
        impl.cfg.max_version > kTlsVersionTls13) {
        impl.fail_local(TlsAlertDesc::InternalError);
        return;
    }

    // PSK offers need the schedule (and its once-only binder key) before the
    // CH bytes exist; the suite comes from the offer, not negotiation.
    if (impl.psk_offered) {
        const TlsSuiteInfo *psk_info = tls_suite_info(session->suite);
        if (psk_info == nullptr || !psk_info->is_tls13) {
            impl.fail_local(TlsAlertDesc::InternalError);
            return;
        }
        impl.sched.emplace(session->suite);
        if (!impl.sched->set_psk(session->psk).has_value()) {
            impl.fail_local(TlsAlertDesc::InternalError);
            return;
        }
    }

    if (!tls_client_hello_build(impl.hello, impl.cfg, session, impl.psk_offered, impl.early.offered_ext, false,
                                static_cast<std::uint16_t>(TlsNamedGroup::X25519), {}) ||
        (impl.psk_offered && !tls_client_backfill_psk_binder(*impl.sched, impl.hello)) ||
        !impl.ctx.emit(TlsContentType::Handshake, {impl.hello.ch.data(), impl.hello.len}).has_value()) {
        impl.fail_local(TlsAlertDesc::InternalError);
        return;
    }
    if (impl.early.offered_ext) {
        // 0-RTT write side (10 定谳 5): TCP builds the early record cipher;
        // QUIC only exports the secret — early data is STREAM frames there
        // and the QUIC layer seals them itself.
        if (impl.cfg.quic != nullptr) {
            auto early_secret = tls_client_early_secret(*impl.sched, impl.hello);
            if (!early_secret.has_value() || !impl.cfg.quic->set_secret(impl.cfg.quic->ctx, TlsQuicLevel::EarlyData,
                                                                        true, session->suite, early_secret->bytes())) {
                impl.fail_local(TlsAlertDesc::InternalError);
                return;
            }
        } else if (!tls_client_init_early_write(*impl.sched, *session, impl.hello, impl.early.write)) {
            impl.fail_local(TlsAlertDesc::InternalError);
            return;
        }
    }
    if (impl.early.offered_ext) {
        // RFC 8446 D.4 / BoringSSL do_enter_early_data: when early data is
        // offered, the compat CCS belongs immediately after the first CH —
        // in front of the 0-RTT flight. From here on records are 0x0303
        // (the peer's record layer checks the version strictly once its read
        // cipher is live, even for the CCS it silently discards).
        impl.ctx.set_legacy_version(kTlsRecordVersionTls12);
        if (!impl.ctx.send_ccs().has_value()) {
            impl.fail_local(TlsAlertDesc::InternalError);
            return;
        }
        impl.ccs_sent = true;
    }
}

TlsClientHandshakeEngine::~TlsClientHandshakeEngine() { delete impl_; }

// =====================================================================
// feed / early data / output / state
// =====================================================================

common::IoResult<TlsClientHandshakeEngine::Event> TlsClientHandshakeEngine::feed(mem::IoBuf &&bytes) noexcept {
    FIBER_ASSERT(impl_ != nullptr && !impl_->out.done);
    if (!impl_->ctx.feed(std::move(bytes))) {
        return std::unexpected(common::IoErr::NoMem);
    }
    return impl_->pump();
}

common::IoResult<TlsClientHandshakeEngine::Event> TlsClientHandshakeEngine::feed(mem::IoBufChain &&bytes) noexcept {
    FIBER_ASSERT(impl_ != nullptr && !impl_->out.done);
    if (!impl_->ctx.feed(std::move(bytes))) {
        return std::unexpected(common::IoErr::NoMem);
    }
    return impl_->pump();
}

common::IoResult<TlsClientHandshakeEngine::Event>
TlsClientHandshakeEngine::feed_quic(TlsQuicLevel level, std::span<const std::uint8_t> bytes) noexcept {
    FIBER_ASSERT(impl_ != nullptr && !impl_->out.done);
    if (!impl_->ctx.provide_quic(level, bytes)) {
        return std::unexpected(common::IoErr::NoMem);
    }
    return impl_->pump();
}

common::IoResult<void> TlsClientHandshakeEngine::write_early_data(std::span<const std::uint8_t> data) noexcept {
    if (impl_ == nullptr || !impl_->early.offered_ext || impl_->early.closed || impl_->out.done ||
        !impl_->early.write.initialized()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (impl_->early.written + data.size() > impl_->session->max_early_data) {
        return std::unexpected(common::IoErr::MessageTooLarge); // nothing written, earlier bytes kept
    }
    const auto emitted = impl_->ctx.emit(TlsContentType::ApplicationData, data, &impl_->early.write);
    if (!emitted.has_value()) {
        return std::unexpected(emitted.error());
    }
    impl_->early.written += data.size();
    return {};
}

mem::IoBufChain TlsClientHandshakeEngine::take_output() noexcept {
    if (impl_ == nullptr) {
        return mem::IoBufChain{};
    }
    return impl_->ctx.take_output();
}

bool TlsClientHandshakeEngine::done() const noexcept { return impl_ == nullptr || impl_->out.done; }

bool TlsClientHandshakeEngine::failed() const noexcept { return impl_ == nullptr || impl_->out.failed; }

TlsAlertDesc TlsClientHandshakeEngine::failure_alert() const noexcept {
    FIBER_ASSERT(done());
    return impl_ != nullptr ? impl_->out.alert : TlsAlertDesc::InternalError;
}

TlsConnectedState TlsClientHandshakeEngine::take_state() noexcept {
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

TlsQuicHandshakeResult TlsClientHandshakeEngine::take_quic_result() noexcept {
    FIBER_ASSERT(impl_ != nullptr && impl_->out.done && !impl_->out.failed);
    FIBER_ASSERT(impl_->cfg.quic != nullptr); // the QUIC-mode twin of take_state
    return std::visit(
            [](auto &sub) -> TlsQuicHandshakeResult {
                using Sub = std::decay_t<decltype(sub)>;
                if constexpr (std::is_same_v<Sub, Tls13ClientHandshake>) {
                    return sub.take_quic_result();
                } else {
                    FIBER_ASSERT(false); // QUIC never mounts monostate-done or the 1.2 sub-flow
                    return TlsQuicHandshakeResult{};
                }
            },
            impl_->flow);
}

mem::IoBufChain TlsClientHandshakeEngine::take_inbound_leftover() noexcept {
    FIBER_ASSERT(impl_ != nullptr && impl_->out.done && !impl_->out.failed);
    return impl_->ctx.take_inbound_leftover();
}

// =====================================================================
// The fork: ServerHello version detection
// =====================================================================

void TlsClientHandshakeEngine::Impl::handle_first_message(TlsHandshakeType type,
                                                          std::span<const std::uint8_t> body) noexcept {
    if (type != TlsHandshakeType::ServerHello) {
        fail_local(TlsAlertDesc::UnexpectedMessage);
        return;
    }
    TlsServerHello sh;
    if (!tls_decode_server_hello(body.data(), body.size(), sh).has_value()) {
        fail_local(TlsAlertDesc::DecodeError);
        return;
    }

    // Version first (06 §4.2): supported_versions present → 1.3 semantics
    // (any other value is illegal_parameter); absent → the 1.2 sub-flow (the
    // CH offers the config window [max, min] descending, so a conforming
    // 1.2-only peer lands there when the window allows it).
    if (sh.has_supported_version && sh.supported_version != kTlsVersionTls13) {
        fail_local(TlsAlertDesc::IllegalParameter);
        return;
    }
    // The other half of the version gate (09 §4.2): a 1.3 negotiation above
    // the config ceiling is fatal protocol_version (the peer ignored our
    // narrowed offer; only reachable with a non-default max).
    if (sh.has_supported_version && cfg.max_version < kTlsVersionTls13) {
        fail_local(TlsAlertDesc::ProtocolVersion);
        return;
    }
    // 0x0303 for everything the client writes from here on — the peer's read
    // cipher is (or is about to be) live and it checks versions strictly,
    // including on alerts and the discarded compat CCS (06 §2.3). True for
    // 1.2 as well: every record after the initial CH flies at 0x0303.
    ctx.set_legacy_version(kTlsRecordVersionTls12);

    if (!sh.has_supported_version) {
        // Version bounds (09 §4.2): a ServerHello without supported_versions
        // negotiates 1.2 — fatal protocol_version when the config floor is
        // above it. (With the default window the CH offered both, so a
        // conforming peer lands here by choice; the gate only bites when the
        // operator narrowed the floor.)
        FIBER_ASSERT(cfg.quic == nullptr); // QUIC pins 1.3 — the gate below fired
        if (cfg.min_version > kTlsVersionTls12) {
            fail_local(TlsAlertDesc::ProtocolVersion);
            return;
        }
        // A 1.2 negotiation kills any PSK/0-RTT offer at the read point;
        // the 1.2 sub-flow never sees the early window.
        early.closed = true;
        Tls12ClientHandshake::Mount mount{ctx, cfg, hello, out, {scratch.data(), scratch.size()}};
        flow.emplace<Tls12ClientHandshake>(mount);
        std::get<Tls12ClientHandshake>(flow).start(sh, body);
        return;
    }

    Tls13ClientHandshake::Mount mount{
            ctx, cfg, session, hello, sched, early, out, {scratch.data(), scratch.size()}, psk_offered, ccs_sent};
    flow.emplace<Tls13ClientHandshake>(mount);
    Tls13ClientHandshake &sub = std::get<Tls13ClientHandshake>(flow);
    if (tls_is_hello_retry_request(sh.random)) {
        sub.start_hello_retry_request(sh, body);
        return;
    }
    sub.start(sh, body);
}

} // namespace fiber::tls
