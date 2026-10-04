#include "TlsServerHandshakeEngine.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>

#include <fiber/tls/TlsVersion.h>
#include <fiber/tls/handshake/TlsExtensionCodec.h>
#include <fiber/tls/handshake/TlsHandshakeCodec.h>
#include <fiber/tls/handshake/TlsHandshakeMessage.h>
#include <fiber/tls/record/TlsRecord.h>

namespace fiber::tls {

// =====================================================================
// The outer shell
// =====================================================================

// The version-neutral half of the engine (07 §4.1) — the mirror of the 06
// client shell with the direction flipped: construction produces NO first
// flight; the first complete ClientHello is the fork point where the version
// is decided and one sub-flow mounts (Tls13ServerHandshake or
// Tls12ServerHandshake). From that fork on, every inbound Message/CCS
// routes to the mounted sub-flow; the sub writes terminal status back
// through `out_`.

void TlsServerHandshakeEngine::fail_local(TlsAlertDesc alert) noexcept {
    if (out_.done) {
        return;
    }
    ctx_.fail(alert); // encodes the fatal alert (sealed when the write cipher is live)
    out_.failed = true;
    out_.done = true;
    out_.alert = alert;
}

void TlsServerHandshakeEngine::fail_peer(TlsAlertDesc alert) noexcept {
    if (out_.done) {
        return;
    }
    out_.failed = true;
    out_.done = true;
    out_.alert = alert; // the peer's alert; nothing is sent back
}

common::IoResult<TlsServerHandshakeEngine::Event> TlsServerHandshakeEngine::pump() noexcept {
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
                // Mid-handshake every inbound alert is terminal (06 §2.3
                // semantics, mirrored) — close_notify included. Nothing is
                // sent back to a connection that already told us it is dying.
                fail_peer(step.alert);
                return Event::Failed;
            case TlsInboundStep::Kind::Ccs:
                // A well-formed client CCS is middlebox compat noise the
                // 1.3 sub-flow ignores (the context validated the 1-byte
                // 0x01 form); the 1.2 sub-flow swaps the read side at its
                // expected point.
                if (auto *sub = std::get_if<Tls13ServerHandshake>(&flow_)) {
                    sub->on_ccs();
                } else if (auto *sub = std::get_if<Tls12ServerHandshake>(&flow_)) {
                    sub->on_ccs();
                }
                break;
            case TlsInboundStep::Kind::Message:
                if (auto *sub = std::get_if<Tls13ServerHandshake>(&flow_)) {
                    sub->on_message(step.type, step.body);
                } else if (auto *sub = std::get_if<Tls12ServerHandshake>(&flow_)) {
                    sub->on_message(step.type, step.body);
                } else {
                    handle_first_message(step.type, step.body);
                }
                break;
        }
    }
}

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
                                                   const TlsServerConfigSource *source) noexcept :
    cfg_(config), resumption_(resumption), minter_(minter), source_(source) {
    // Server plaintext records are 0x0303 from the very first one (the
    // client's 0x0301 first-flight convention is client-only, 06 §2.3).
    ctx_.set_legacy_version(kTlsRecordVersionTls12);

    if (cfg_.quic != nullptr) {
        // QUIC (10 §3): records never frame; the HRR (if any) rides Initial
        // CRYPTO, the SH flight Handshake. The context binds the template's
        // callbacks here — a per-ClientHello selector may not change them
        // (asserted at the fork).
        FIBER_ASSERT(cfg_.min_version == kTlsVersionTls13 && cfg_.max_version == kTlsVersionTls13);
        ctx_.enable_quic(*cfg_.quic);
    }

    // Configuration invariants at the boundary (no PSK-only mode exists).
    // With a per-ClientHello selector the ctor config is a TEMPLATE:
    // chain/key arrive with the selection (checked at the fork); only the
    // version window and the mTLS coupling must already hold.
    const bool selected_per_hello = source != nullptr && source->select != nullptr;
    if (!((selected_per_hello && config_versions_hold(cfg_)) || config_invariants_hold(cfg_))) {
        fail_local(TlsAlertDesc::InternalError); // configuration bug, not a protocol event
        return;
    }
}

// =====================================================================
// feed / early data / output / state
// =====================================================================

common::IoResult<TlsServerHandshakeEngine::Event> TlsServerHandshakeEngine::feed(mem::IoBuf &&bytes) noexcept {
    FIBER_ASSERT(!out_.done);
    if (!ctx_.feed(std::move(bytes))) {
        return std::unexpected(common::IoErr::NoMem);
    }
    return pump();
}

common::IoResult<TlsServerHandshakeEngine::Event> TlsServerHandshakeEngine::feed(mem::IoBufChain &&bytes) noexcept {
    FIBER_ASSERT(!out_.done);
    if (!ctx_.feed(std::move(bytes))) {
        return std::unexpected(common::IoErr::NoMem);
    }
    return pump();
}

common::IoResult<TlsServerHandshakeEngine::Event> TlsServerHandshakeEngine::feed_quic(TlsQuicLevel level,
                                                                                      mem::IoBufChain &bytes) noexcept {
    FIBER_ASSERT(!out_.done);
    if (!ctx_.provide_quic(level, bytes)) {
        return std::unexpected(common::IoErr::NoMem);
    }
    return pump();
}

mem::IoBufChain TlsServerHandshakeEngine::take_early_data() noexcept { return std::move(early_); }

mem::IoBufChain TlsServerHandshakeEngine::take_output() noexcept { return ctx_.take_output(); }

bool TlsServerHandshakeEngine::done() const noexcept { return out_.done; }

bool TlsServerHandshakeEngine::failed() const noexcept { return out_.failed; }

TlsAlertDesc TlsServerHandshakeEngine::failure_alert() const noexcept {
    FIBER_ASSERT(done());
    return out_.alert;
}

TlsConnectedState TlsServerHandshakeEngine::take_state() noexcept {
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

TlsQuicHandshakeResult TlsServerHandshakeEngine::take_quic_result() noexcept {
    FIBER_ASSERT(out_.done && !out_.failed);
    FIBER_ASSERT(cfg_.quic != nullptr); // the QUIC-mode twin of take_state
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
            flow_);
}

mem::IoBufChain TlsServerHandshakeEngine::take_inbound_leftover() noexcept {
    FIBER_ASSERT(out_.done && !out_.failed);
    return ctx_.take_inbound_leftover();
}

// =====================================================================
// The fork: ClientHello version detection
// =====================================================================

void TlsServerHandshakeEngine::handle_first_message(TlsHandshakeType type,
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
    std::memcpy(hello_.ch.data(), body.data(), body.size());
    hello_.ch_len = body.size();
    if (!tls_decode_client_hello(hello_.ch.data(), hello_.ch_len, hello_.view).has_value()) {
        fail_local(TlsAlertDesc::DecodeError);
        return;
    }
    const TlsClientHello &ch = hello_.view;

    // Per-ClientHello config selection (09 §4.1): the selector runs right
    // after the decode, before any version/policy decision — the returned
    // config drives THIS connection. The spans it hands back borrow caller
    // staging that outlives the engine; null = the hello selects none (an
    // SNI this host does not serve) → handshake_failure.
    if (source_ != nullptr && source_->select != nullptr) {
        const TlsServerConfig *selected = source_->select(source_->ctx, ch);
        if (selected == nullptr || !config_invariants_hold(*selected)) {
            fail_local(TlsAlertDesc::HandshakeFailure);
            return;
        }
        FIBER_ASSERT(selected->quic == cfg_.quic); // the context is QUIC-bound to the template's callbacks
        cfg_ = *selected;
    }

    if (cfg_.quic != nullptr) {
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
            cfg_.quic->on_peer_transport_params(cfg_.quic->ctx, params);
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
    const bool allow13 = offers13 && cfg_.max_version >= kTlsVersionTls13;
    const bool allow12 = offers12 && cfg_.min_version <= kTlsVersionTls12 && cfg_.max_version >= kTlsVersionTls12 &&
                         cfg_.quic == nullptr; // QUIC is 1.3-only (10 §3.4) — a 1.2 fork cannot mount

    if (allow13) {
        if (!null_compression) {
            fail_local(TlsAlertDesc::IllegalParameter);
            return;
        }
        auto &sub = flow_.emplace<Tls13ServerHandshake>(Tls13ServerHandshake::Mount{
                ctx_, cfg_, resumption_, minter_, hello_, early_, out_, {scratch_.data(), scratch_.size()}});
        sub.start(ch, {hello_.ch.data(), hello_.ch_len});
        return;
    }
    if (allow12) {
        if (!null_compression) {
            fail_local(TlsAlertDesc::HandshakeFailure); // RFC 5246 §7.4.1.4
            return;
        }
        FIBER_ASSERT(cfg_.quic == nullptr); // the allow12 gate above is QUIC-blind by construction
        auto &sub = flow_.emplace<Tls12ServerHandshake>(Tls12ServerHandshake::Mount{
                ctx_, cfg_, resumption_, minter_, hello_, out_, {scratch_.data(), scratch_.size()}});
        sub.start(ch, {hello_.ch.data(), hello_.ch_len});
        return;
    }
    fail_local(TlsAlertDesc::ProtocolVersion);
}

} // namespace fiber::tls
