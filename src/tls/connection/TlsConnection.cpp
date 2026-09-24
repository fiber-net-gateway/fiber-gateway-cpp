#include <fiber/tls/TlsConnection.h>

#include <cstring>
#include <new>
#include <utility>

#include <fiber/tls/crypto/Tls13KeySchedule.h>
#include "../detail/TlsHandshakeContext.h"

namespace fiber::tls {

namespace {
constexpr std::uint8_t kAlertLevelWarning = 1;
} // namespace

struct TlsConnection::Impl {
    Impl(TlsConnectionRole role, TlsConnectedState &&state) noexcept;

    // Post-handshake handshake-message dispatch (1.3 NST/KeyUpdate; 1.2 all
    // fatal). False = terminal latched, pump returns.
    [[nodiscard]] bool dispatch_post_handshake(TlsHandshakeType type, std::span<const std::uint8_t> body) noexcept;
    [[nodiscard]] bool handle_key_update(std::span<const std::uint8_t> body) noexcept;

    // Sends the armed KeyUpdate response (RFC 8446 §4.6.1 MUST): the message
    // itself under the CURRENT write keys, the rotation applying to the
    // records after it. Everything that can fail is staged before any state
    // moves. No-op when nothing is armed.
    [[nodiscard]] common::IoResult<void> send_pending_rekey() noexcept;

    // Latches a terminal and encodes our fatal alert into the outbound
    // chain (sealed when the write cipher lives — the glue flushes
    // best-effort, then tears down).
    void latch_fatal(TlsAlertDesc alert) noexcept;

    [[nodiscard]] common::IoResult<void> write_guard() noexcept;

    TlsHandshakeContext ctx_;
    mem::IoBufChain plaintext_; // app-data sink target (ctx_ holds the pointer)
    TlsSecret own_secret_; // 1.3 write-side rekey base; empty on 1.2
    TlsSecret peer_secret_; // 1.3 read-side rekey base; empty on 1.2
    std::array<std::uint8_t, 256> alpn_{};
    TlsProtocolVersion version_;
    TlsCipherSuiteId suite_;
    TlsAlertDesc failure_alert_ = TlsAlertDesc::InternalError;
    std::uint16_t alpn_len_ = 0;
    bool rekey_pending_ = false; // peer's KeyUpdate(update_requested): owe a response before the next write
    bool peer_closed_ = false; // the peer's close_notify latched
    bool close_sent_ = false; // our close_notify encoded
    bool failed_ = false; // terminal latched (ours or the peer's)
};

TlsConnection::Impl::Impl(TlsConnectionRole role, TlsConnectedState &&state) noexcept :
    version_(state.version), suite_(state.suite) {
    alpn_len_ = state.alpn_len;
    std::memcpy(alpn_.data(), state.alpn.data(), state.alpn_len);
    // Role picks which app secret is ours (write-side rekey base) — the
    // ciphers themselves are already endpoint-oriented by the engines' move.
    own_secret_ = std::move(role == TlsConnectionRole::Client ? state.client_app_secret : state.server_app_secret);
    peer_secret_ = std::move(role == TlsConnectionRole::Client ? state.server_app_secret : state.client_app_secret);
    ctx_.read_cipher() = std::move(state.read_cipher);
    ctx_.write_cipher() = std::move(state.write_cipher);
    ctx_.set_inbound_mode(version_ == TlsProtocolVersion::Tls13 ? TlsInboundMode::Sealed13 : TlsInboundMode::Sealed12);
    ctx_.arm_app_data_sink(plaintext_);
}

bool TlsConnection::Impl::dispatch_post_handshake(TlsHandshakeType type, std::span<const std::uint8_t> body) noexcept {
    if (version_ != TlsProtocolVersion::Tls13) {
        // 1.2 has no post-handshake message we accept: HelloRequest
        // (renegotiation) is refused outright (09 §1 decision 3), and a
        // renegotiation ClientHello is equally out of scope.
        latch_fatal(TlsAlertDesc::UnexpectedMessage);
        return false;
    }
    switch (type) {
        case TlsHandshakeType::NewSessionTicket:
            // The stateless server's tickets the peer keeps sending:
            // swallowed — the client keeps no session cache (09 §1 decision
            // 1). Size-bounded by the reassembler cap.
            return true;
        case TlsHandshakeType::KeyUpdate:
            return handle_key_update(body);
        default:
            // CertificateRequest and friends: not supported.
            latch_fatal(TlsAlertDesc::UnexpectedMessage);
            return false;
    }
}

bool TlsConnection::Impl::handle_key_update(std::span<const std::uint8_t> body) noexcept {
    // RFC 8446 §4.6.1: exactly one byte, update_not_requested(0) or
    // update_requested(1). Malformed either way → decode_error (BoringSSL's
    // alert choice — kept identical for interop).
    if (body.size() != 1 || body[0] > 1) {
        latch_fatal(TlsAlertDesc::DecodeError);
        return false;
    }
    // Read side rekeys NOW: the records after the KeyUpdate arrive under the
    // next traffic-secret generation. Stage everything that can fail before
    // any state moves.
    auto next = tls13_key_update(peer_secret_);
    if (!next.has_value()) {
        latch_fatal(TlsAlertDesc::InternalError);
        return false;
    }
    auto keys = tls13_traffic_keys(*next, suite_);
    if (!keys.has_value()) {
        latch_fatal(TlsAlertDesc::InternalError);
        return false;
    }
    TlsRecordCipher fresh;
    if (!fresh.init(suite_, TlsRecordProtectionKind::Tls13, {keys->key.data(), keys->key_len},
                    {keys->iv.data(), keys->iv_len})
                 .has_value()) {
        latch_fatal(TlsAlertDesc::InternalError);
        return false;
    }
    peer_secret_ = std::move(*next);
    ctx_.read_cipher() = std::move(fresh);
    if (body[0] == 1) {
        // The passive response (RFC MUST): own KeyUpdate before the next
        // write/close_notify. This is a response, never an initiation — we
        // never send KeyUpdate otherwise (09 §1 decision 2).
        rekey_pending_ = true;
    }
    return true;
}

common::IoResult<void> TlsConnection::Impl::send_pending_rekey() noexcept {
    if (!rekey_pending_) {
        return {};
    }
    auto next = tls13_key_update(own_secret_);
    if (!next.has_value()) {
        return std::unexpected(next.error());
    }
    auto keys = tls13_traffic_keys(*next, suite_);
    if (!keys.has_value()) {
        return std::unexpected(keys.error());
    }
    TlsRecordCipher fresh;
    auto init = fresh.init(suite_, TlsRecordProtectionKind::Tls13, {keys->key.data(), keys->key_len},
                           {keys->iv.data(), keys->iv_len});
    if (!init.has_value()) {
        return init;
    }
    // The response flies under the CURRENT write keys — the peer rotates on
    // receipt (RFC 8446 §4.6.1); our rotation applies to the records after.
    const std::uint8_t key_update[5] = {static_cast<std::uint8_t>(TlsHandshakeType::KeyUpdate), 0, 0, 1, 0};
    auto emitted = ctx_.emit(TlsContentType::Handshake, key_update, nullptr);
    if (!emitted.has_value()) {
        return emitted;
    }
    own_secret_ = std::move(*next);
    ctx_.write_cipher() = std::move(fresh);
    rekey_pending_ = false;
    return {};
}

void TlsConnection::Impl::latch_fatal(TlsAlertDesc alert) noexcept {
    failed_ = true;
    failure_alert_ = alert;
    ctx_.fail(alert); // idempotent; NoMem leaves failed_ set without bytes
}

common::IoResult<void> TlsConnection::Impl::write_guard() noexcept {
    if (failed_) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (close_sent_) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return send_pending_rekey();
}

// =====================================================================
// public shell
// =====================================================================

TlsConnection::TlsConnection(TlsConnectionRole role, TlsConnectedState &&state) noexcept :
    impl_(new (std::nothrow) Impl(role, std::move(state))) {}

TlsConnection::~TlsConnection() { delete impl_; }

bool TlsConnection::feed(mem::IoBuf &&bytes) noexcept {
    if (impl_ == nullptr) {
        return false;
    }
    return impl_->ctx_.feed(std::move(bytes));
}

bool TlsConnection::feed(mem::IoBufChain &&bytes) noexcept {
    if (impl_ == nullptr) {
        return false;
    }
    return impl_->ctx_.feed(std::move(bytes));
}

void TlsConnection::pump() noexcept {
    if (impl_ == nullptr || impl_->failed_ || impl_->peer_closed_) {
        return;
    }
    for (;;) {
        const TlsInboundStep step = impl_->ctx_.step();
        switch (step.kind) {
            case TlsInboundStep::Kind::NeedMore:
                return;
            case TlsInboundStep::Kind::Alert:
                if (step.close_notify) {
                    // Terminal, no response owed; plaintext delivered before
                    // the alert stays readable.
                    impl_->peer_closed_ = true;
                } else {
                    // The peer's fatal alert: terminal, nothing to send.
                    impl_->failed_ = true;
                    impl_->failure_alert_ = step.alert;
                }
                return;
            case TlsInboundStep::Kind::Ccs:
                // Post-handshake CCS has no legal meaning in either version:
                // 1.3's middlebox-compat window is handshake-only (RFC 8446
                // §5), and 1.2 renegotiation is out of scope (09 §1).
                impl_->latch_fatal(TlsAlertDesc::UnexpectedMessage);
                return;
            case TlsInboundStep::Kind::Fatal:
                impl_->latch_fatal(step.alert);
                return;
            case TlsInboundStep::Kind::Message:
                if (!impl_->dispatch_post_handshake(step.type, step.body)) {
                    return;
                }
                break;
        }
    }
}

TlsConnection::ReadStatus TlsConnection::read(void *buf, std::size_t len, std::size_t &out_len) noexcept {
    out_len = 0;
    if (impl_ == nullptr) {
        return ReadStatus::Fatal;
    }
    const std::size_t available = impl_->plaintext_.readable_bytes();
    if (len == 0 || available == 0) {
        if (impl_->failed_) {
            return ReadStatus::Fatal;
        }
        if (impl_->peer_closed_) {
            return ReadStatus::PeerClosed;
        }
        return ReadStatus::NeedMore;
    }
    const std::size_t take = len < available ? len : available;
    std::size_t done = 0;
    while (done < take) {
        mem::IoBuf *front = impl_->plaintext_.first_readable();
        if (front == nullptr) {
            break; // readable_bytes() and the chain agree by construction
        }
        const std::size_t n = front->readable() < take - done ? front->readable() : take - done;
        std::memcpy(static_cast<std::uint8_t *>(buf) + done, front->readable_data(), n);
        impl_->plaintext_.consume_and_compact(n);
        done += n;
    }
    out_len = done;
    return ReadStatus::Ok;
}

std::size_t TlsConnection::pending_plaintext() const noexcept {
    return impl_ == nullptr ? 0 : impl_->plaintext_.readable_bytes();
}

common::IoResult<void> TlsConnection::write(std::span<const std::uint8_t> payload) noexcept {
    if (impl_ == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }
    const auto guard = impl_->write_guard();
    if (!guard.has_value()) {
        return guard;
    }
    const auto emitted = impl_->ctx_.emit(TlsContentType::ApplicationData, payload, nullptr);
    return emitted;
}

common::IoResult<void> TlsConnection::write(mem::IoBufChain &&payload) noexcept {
    if (impl_ == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }
    const auto guard = impl_->write_guard();
    if (!guard.has_value()) {
        return guard;
    }
    // Node-wise emission: emit() splits each contiguous span into
    // <= 16 KiB records itself.
    for (const mem::IoBufNode *node = payload.front_node(); node != nullptr; node = node->next) {
        const std::size_t readable = node->buf.readable();
        if (readable == 0) {
            continue;
        }
        const auto emitted =
                impl_->ctx_.emit(TlsContentType::ApplicationData, {node->buf.readable_data(), readable}, nullptr);
        if (!emitted.has_value()) {
            return emitted;
        }
    }
    return {};
}

common::IoResult<void> TlsConnection::close_notify() noexcept {
    if (impl_ == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }
    if (impl_->close_sent_) {
        return {};
    }
    if (impl_->failed_) {
        return std::unexpected(common::IoErr::Invalid);
    }
    const auto rekey = impl_->send_pending_rekey();
    if (!rekey.has_value()) {
        return rekey;
    }
    const std::uint8_t alert[2] = {kAlertLevelWarning, static_cast<std::uint8_t>(TlsAlertDesc::CloseNotify)};
    const auto emitted = impl_->ctx_.emit(TlsContentType::Alert, alert, nullptr);
    if (!emitted.has_value()) {
        return emitted;
    }
    impl_->close_sent_ = true;
    return {};
}

mem::IoBufChain TlsConnection::take_output() noexcept {
    if (impl_ == nullptr) {
        return mem::IoBufChain{};
    }
    return impl_->ctx_.take_output();
}

bool TlsConnection::peer_closed() const noexcept { return impl_ != nullptr && impl_->peer_closed_; }

bool TlsConnection::failed() const noexcept { return impl_ == nullptr || impl_->failed_; }

TlsAlertDesc TlsConnection::failure_alert() const noexcept {
    return impl_ != nullptr ? impl_->failure_alert_ : TlsAlertDesc::InternalError;
}

std::span<const std::uint8_t> TlsConnection::alpn() const noexcept {
    return impl_ == nullptr ? std::span<const std::uint8_t>{}
                            : std::span<const std::uint8_t>{impl_->alpn_.data(), impl_->alpn_len_};
}

} // namespace fiber::tls
