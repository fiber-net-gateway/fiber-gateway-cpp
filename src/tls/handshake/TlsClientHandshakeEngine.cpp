#include "TlsClientHandshakeEngine.h"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>

#include <fiber/tls/TlsVersion.h>
#include <fiber/tls/handshake/TlsHandshakeCodec.h>
#include <fiber/tls/handshake/TlsHandshakeMessage.h>
#include <fiber/tls/record/TlsRecord.h>

namespace fiber::tls {

// =====================================================================
// The outer shell
// =====================================================================

// The version-neutral half of the engine (06 §4.1): the first flight
// (ClientHello ± PSK binder ± compat CCS ± the 0-RTT window), the record
// pipeline, and the ServerHello read point where the version forks. From
// that fork on, `flow_` holds exactly one sub-flow — Tls13ClientHandshake or
// Tls12ClientHandshake — and every inbound Message/CCS routes to it; the
// sub writes terminal status back through `out_`.

void TlsClientHandshakeEngine::fail_local(TlsAlertDesc alert) noexcept {
    if (out_.done) {
        return;
    }
    ctx_.fail(alert); // encodes the fatal alert (sealed when the write cipher is live)
    out_.failed = true;
    out_.done = true;
    out_.alert = alert;
}

void TlsClientHandshakeEngine::fail_peer(TlsAlertDesc alert) noexcept {
    if (out_.done) {
        return;
    }
    out_.failed = true;
    out_.done = true;
    out_.alert = alert; // the peer's alert; nothing is sent back
}

common::IoResult<TlsClientHandshakeEngine::Event> TlsClientHandshakeEngine::pump() noexcept {
    for (;;) {
        if (out_.done) {
            return out_.failed ? Event::Failed : Event::HandshakeDone;
        }
        const TlsInboundStep step = ctx_.step();
        switch (step.kind) {
            case TlsInboundStep::Kind::NeedMore:
                return Event::None;
            case TlsInboundStep::Kind::Fatal:
                fail_local(step.alert);
                return Event::Failed;
            case TlsInboundStep::Kind::Alert:
                // Mid-handshake every inbound alert is terminal (06 §2.3) —
                // close_notify included: the peer walked away mid-flight. Nothing
                // is sent back to a connection that already told us it is dying.
                fail_peer(step.alert);
                return Event::Failed;
            case TlsInboundStep::Kind::Ccs:
                // Pre-fork and 1.3: a well-formed peer CCS is middlebox
                // compat noise the client ignores (the context validated
                // the 1-byte 0x01 form). Only the 1.2 sub-flow reacts.
                if (auto *sub = std::get_if<Tls12ClientHandshake>(&flow_)) {
                    sub->on_ccs();
                }
                break;
            case TlsInboundStep::Kind::Message:
                if (auto *sub13 = std::get_if<Tls13ClientHandshake>(&flow_)) {
                    sub13->on_message(step.type, step.body);
                } else if (auto *sub12 = std::get_if<Tls12ClientHandshake>(&flow_)) {
                    sub12->on_message(step.type, step.body);
                } else {
                    handle_first_message(step.type, step.body);
                }
                break;
        }
    }
}

// =====================================================================
// ClientHello first flight
// =====================================================================

TlsClientHandshakeEngine::TlsClientHandshakeEngine(const TlsClientConfig &config,
                                                   const TlsSessionOffer *session) noexcept :
    cfg_(config), session_(session) {
    ctx_.set_legacy_version(kTlsRecordVersionTls10); // first flight (06 §2.3)
    // The client talks to a server it chose: every in-handshake message may
    // use the chain-sized cap (BoringSSL client parity, see the ctx caps).
    ctx_.set_max_handshake_message(TlsHandshakeContext::kMaxCertificateMessage);

    if (cfg_.quic != nullptr) {
        // QUIC (10 §3): records never frame; the CH rides Initial-level
        // CRYPTO. The glue pins the version window to 1.3 (QUIC is 1.3-only)
        // — asserted here, enforced at the fork by the 09 version gates.
        FIBER_ASSERT(cfg_.min_version == kTlsVersionTls13 && cfg_.max_version == kTlsVersionTls13);
        ctx_.enable_quic(*cfg_.quic);
        // ALPN is mandatory in QUIC (RFC 9001 §8.1) — BoringSSL refuses the
        // CH build with an empty offer list; answer the config bug up front.
        if (cfg_.alpn.empty()) {
            fail_local(TlsAlertDesc::InternalError);
            return;
        }
    }

    psk_offered_ = session != nullptr;
    early_.offered_ext = psk_offered_ && session->max_early_data > 0;

    if (cfg_.verify_peer && cfg_.trust == nullptr) {
        fail_local(TlsAlertDesc::InternalError); // configuration bug, not a protocol event
        return;
    }
    // Version bounds invariant (09 §4.2): the domain is {1.2, 1.3} (1.3 is
    // the implementation ceiling) and the window must be non-empty. Like the
    // trust check above, this is a configuration bug — answer it up front,
    // not as a mystery failure after the first flight.
    if (cfg_.min_version > cfg_.max_version || cfg_.min_version < kTlsVersionTls12 ||
        cfg_.max_version > kTlsVersionTls13) {
        fail_local(TlsAlertDesc::InternalError);
        return;
    }

    // PSK offers need the schedule (and its once-only binder key) before the
    // CH bytes exist; the suite comes from the offer, not negotiation.
    if (psk_offered_) {
        const TlsSuiteInfo *psk_info = tls_suite_info(session->suite);
        if (psk_info == nullptr || !psk_info->is_tls13) {
            fail_local(TlsAlertDesc::InternalError);
            return;
        }
        sched_.emplace(session->suite);
        if (!sched_->set_psk(session->psk).has_value()) {
            fail_local(TlsAlertDesc::InternalError);
            return;
        }
    }

    if (!tls_client_hello_build(hello_, cfg_, session, psk_offered_, early_.offered_ext, false,
                                static_cast<std::uint16_t>(TlsNamedGroup::X25519), {}) ||
        (psk_offered_ && !tls_client_backfill_psk_binder(*sched_, hello_)) ||
        !ctx_.emit(TlsContentType::Handshake, {hello_.ch.data(), hello_.len}).has_value()) {
        fail_local(TlsAlertDesc::InternalError);
        return;
    }
    if (early_.offered_ext) {
        // 0-RTT write side (10 定谳 5): TCP builds the early record cipher;
        // QUIC only exports the secret — early data is STREAM frames there
        // and the QUIC layer seals them itself.
        if (cfg_.quic != nullptr) {
            auto early_secret = tls_client_early_secret(*sched_, hello_);
            if (!early_secret.has_value() || !cfg_.quic->set_secret(cfg_.quic->ctx, TlsQuicLevel::EarlyData, true,
                                                                    session->suite, early_secret->bytes())) {
                fail_local(TlsAlertDesc::InternalError);
                return;
            }
        } else if (!tls_client_init_early_write(*sched_, *session, hello_, early_.write)) {
            fail_local(TlsAlertDesc::InternalError);
            return;
        }
    }
    if (early_.offered_ext) {
        // RFC 8446 D.4 / BoringSSL do_enter_early_data: when early data is
        // offered, the compat CCS belongs immediately after the first CH —
        // in front of the 0-RTT flight. From here on records are 0x0303
        // (the peer's record layer checks the version strictly once its read
        // cipher is live, even for the CCS it silently discards).
        ctx_.set_legacy_version(kTlsRecordVersionTls12);
        if (!ctx_.send_ccs().has_value()) {
            fail_local(TlsAlertDesc::InternalError);
            return;
        }
        ccs_sent_ = true;
    }
}

// =====================================================================
// feed / early data / output / state
// =====================================================================

common::IoResult<TlsClientHandshakeEngine::Event> TlsClientHandshakeEngine::feed(mem::IoBuf &&bytes) noexcept {
    FIBER_ASSERT(!out_.done);
    if (!ctx_.feed(std::move(bytes))) {
        return std::unexpected(common::IoErr::NoMem);
    }
    return pump();
}

common::IoResult<TlsClientHandshakeEngine::Event> TlsClientHandshakeEngine::feed(mem::IoBufChain &&bytes) noexcept {
    FIBER_ASSERT(!out_.done);
    if (!ctx_.feed(std::move(bytes))) {
        return std::unexpected(common::IoErr::NoMem);
    }
    return pump();
}

common::IoResult<TlsClientHandshakeEngine::Event> TlsClientHandshakeEngine::feed_quic(TlsQuicLevel level,
                                                                                      mem::IoBufChain &bytes) noexcept {
    FIBER_ASSERT(!out_.done);
    if (!ctx_.provide_quic(level, bytes)) {
        return std::unexpected(common::IoErr::NoMem);
    }
    return pump();
}

common::IoResult<void> TlsClientHandshakeEngine::write_early_data(std::span<const std::uint8_t> data) noexcept {
    if (!early_.offered_ext || early_.closed || out_.done || !early_.write.initialized()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (early_.written + data.size() > session_->max_early_data) {
        return std::unexpected(common::IoErr::MessageTooLarge); // nothing written, earlier bytes kept
    }
    const auto emitted = ctx_.emit(TlsContentType::ApplicationData, data, &early_.write);
    if (!emitted.has_value()) {
        return std::unexpected(emitted.error());
    }
    early_.written += data.size();
    return {};
}

mem::IoBufChain TlsClientHandshakeEngine::take_output() noexcept { return ctx_.take_output(); }

bool TlsClientHandshakeEngine::done() const noexcept { return out_.done; }

bool TlsClientHandshakeEngine::failed() const noexcept { return out_.failed; }

TlsAlertDesc TlsClientHandshakeEngine::failure_alert() const noexcept {
    FIBER_ASSERT(done());
    return out_.alert;
}

TlsConnectedState TlsClientHandshakeEngine::take_state() noexcept {
    FIBER_ASSERT(out_.done && !out_.failed);
    FIBER_ASSERT(cfg_.quic == nullptr); // QUIC hands over via take_quic_result (10 定谳 2)
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
            flow_);
}

TlsQuicHandshakeResult TlsClientHandshakeEngine::take_quic_result() noexcept {
    FIBER_ASSERT(out_.done && !out_.failed);
    FIBER_ASSERT(cfg_.quic != nullptr); // the QUIC-mode twin of take_state
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
            flow_);
}

mem::IoBufChain TlsClientHandshakeEngine::take_inbound_leftover() noexcept {
    FIBER_ASSERT(out_.done && !out_.failed);
    return ctx_.take_inbound_leftover();
}

// =====================================================================
// The fork: ServerHello version detection
// =====================================================================

void TlsClientHandshakeEngine::handle_first_message(TlsHandshakeType type,
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
    if (sh.has_supported_version && cfg_.max_version < kTlsVersionTls13) {
        fail_local(TlsAlertDesc::ProtocolVersion);
        return;
    }
    // 0x0303 for everything the client writes from here on — the peer's read
    // cipher is (or is about to be) live and it checks versions strictly,
    // including on alerts and the discarded compat CCS (06 §2.3). True for
    // 1.2 as well: every record after the initial CH flies at 0x0303.
    ctx_.set_legacy_version(kTlsRecordVersionTls12);

    if (!sh.has_supported_version) {
        // Version bounds (09 §4.2): a ServerHello without supported_versions
        // negotiates 1.2 — fatal protocol_version when the config floor is
        // above it. (With the default window the CH offered both, so a
        // conforming peer lands here by choice; the gate only bites when the
        // operator narrowed the floor.) A QUIC peer reaches this too: the
        // ServerHello is peer-controlled, and QUIC's pinned 1.3 floor makes
        // the gate fire (RFC 9001 §4.2).
        if (cfg_.min_version > kTlsVersionTls12) {
            fail_local(TlsAlertDesc::ProtocolVersion);
            return;
        }
        FIBER_ASSERT(cfg_.quic == nullptr); // QUIC pins 1.3 — the gate above fired
        // A 1.2 negotiation kills any PSK/0-RTT offer at the read point;
        // the 1.2 sub-flow never sees the early window.
        early_.closed = true;
        Tls12ClientHandshake::Mount mount{ctx_, cfg_, hello_, out_, {scratch_.data(), scratch_.size()}};
        flow_.emplace<Tls12ClientHandshake>(mount);
        std::get<Tls12ClientHandshake>(flow_).start(sh, body);
        return;
    }

    Tls13ClientHandshake::Mount mount{ctx_,         cfg_,     session_, hello_,
                                      sched_,       early_,   out_,     {scratch_.data(), scratch_.size()},
                                      psk_offered_, ccs_sent_};
    flow_.emplace<Tls13ClientHandshake>(mount);
    Tls13ClientHandshake &sub = std::get<Tls13ClientHandshake>(flow_);
    if (tls_is_hello_retry_request(sh.random)) {
        sub.start_hello_retry_request(sh, body);
        return;
    }
    sub.start(sh, body);
}

} // namespace fiber::tls
