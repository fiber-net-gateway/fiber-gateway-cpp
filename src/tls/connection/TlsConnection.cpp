#include <fiber/tls/TlsConnection.h>

#include <algorithm>
#include <cstring>
#include <new>
#include <utility>

#include <fiber/common/Assert.h>
#include <fiber/tls/crypto/Tls13KeySchedule.h>
#include <fiber/tls/handshake/TlsHandshakeMessage.h>
#include <fiber/tls/record/TlsRecordCipherChain.h>

namespace fiber::tls {

namespace {

constexpr std::uint8_t kAlertLevelFatal = 2;
constexpr std::uint8_t kAlertLevelWarning = 1;

// Post-handshake handshake-message cap: NSTs can straddle records;
// KeyUpdate is 4+1 bytes. 16 KiB = BoringSSL's kMaxMessageLen — either peer
// can send these at any time, so the bound caps pinned reassembly memory.
constexpr std::size_t kMaxPostHandshakeMessage = 16u << 10;

// 1.2 consecutive warning alerts tolerated (BoringSSL kMaxWarningAlerts).
constexpr std::uint8_t kMaxWarningAlerts = 4;

// Worst-case open workspace: one record, straddling topology (the gather
// destination when the body is not contiguous in the record's chain).
constexpr std::size_t kOpenScratchSize = kTlsMaxCiphertextRecordSize;

// Gathers `out.size()` bytes off a chain's readable prefix into `out`
// (straddling records can split them across nodes); nullptr when the chain
// holds fewer bytes.
const std::uint8_t *gather_small(const mem::IoBufChain &chain, std::span<std::uint8_t> out) noexcept {
    std::size_t done = 0;
    for (const mem::IoBufNode *node = chain.front_node(); node != nullptr && done < out.size(); node = node->next) {
        const mem::IoBuf &buf = node->buf;
        const std::size_t take = std::min(buf.readable(), out.size() - done);
        if (take > 0) {
            std::memcpy(out.data() + done, buf.readable_data(), take);
            done += take;
        }
    }
    return done == out.size() ? out.data() : nullptr;
}

} // namespace

struct TlsConnection::Impl {
    Impl(TlsConnectionRole role, TlsConnectedState &&state) noexcept;

    // Post-handshake handshake-message dispatch (1.3 NST/KeyUpdate; 1.2 all
    // fatal). False = terminal latched, fragment processing stops.
    [[nodiscard]] bool dispatch_post_handshake(TlsHandshakeType type, std::span<const std::uint8_t> body) noexcept;
    [[nodiscard]] bool handle_key_update(std::span<const std::uint8_t> body) noexcept;

    // Sends the armed KeyUpdate response (RFC 8446 §4.6.1 MUST): the message
    // itself under the CURRENT write keys, the rotation applying to the
    // records after it. Everything that can fail is staged before any state
    // moves. No-op when nothing is armed.
    [[nodiscard]] common::IoResult<void> send_pending_rekey() noexcept;

    // Latches a terminal and encodes our fatal alert into the outbound
    // chain (sealed — the write cipher always lives here; the glue flushes
    // best-effort, then tears down). Idempotent; NoMem while encoding
    // leaves failed_ set without bytes (the connection dies either way).
    void latch_fatal(TlsAlertDesc alert) noexcept;

    [[nodiscard]] common::IoResult<void> write_guard() noexcept;

    // ---- inbound record pipeline ----

    // Outer-type dispatch (the mode is fixed: sealed, per version).
    void route_record(TlsRecord &&record) noexcept;
    // Opens the sealed body in place / via the scratch and routes the inner
    // content type. 1.2 preserves the outer type under encryption (the
    // AAD-bound inner type IS it); 1.3 routes the decrypted trailing type.
    void open_and_route(TlsRecord &record) noexcept;
    // Two decoded alert bytes (severity, description).
    void on_alert_bytes(const std::uint8_t *bytes) noexcept;
    // App data delivery: in_chain moves the record's shrunk plaintext view
    // into the delivery chain (zero copies); otherwise the plaintext sits
    // in the open scratch and is copied out (the one gather the AEAD's
    // single-input-region contract costs).
    void deliver_plaintext(TlsRecord &record, bool in_chain, std::size_t plain_len) noexcept;
    // Post-handshake handshake-message reassembly across records, then
    // dispatch per complete message (borrowed bodies).
    void feed_handshake_fragment(const std::uint8_t *frag, std::size_t len) noexcept;
    [[nodiscard]] bool append_fragment(std::span<const std::uint8_t> bytes) noexcept;
    [[nodiscard]] bool materialize_message(std::size_t message_len) noexcept;
    // 1.3 rewrites every sealed record's outer type to application_data;
    // 1.2 preserves the payload's true type under encryption (the AAD binds
    // it either way). Both fly at 0x0303.
    [[nodiscard]] TlsContentType outer_record_type(TlsContentType type) const noexcept {
        return version_ == TlsProtocolVersion::Tls13 ? TlsContentType::ApplicationData : type;
    }
    // Frames payload as sealed records into out_ (<=14KiB per record; 1.3
    // outer application_data, 1.2 type-preserved, both at 0x0303; empty
    // payload → one zero-length record).
    [[nodiscard]] common::IoResult<void> emit_sealed(TlsContentType type,
                                                     std::span<const std::uint8_t> payload) noexcept;

    TlsRecordCipher read_cipher_;
    TlsRecordCipher write_cipher_;
    mem::IoBufChain out_{};
    mem::IoBufChain plaintext_{}; // delivered app data awaiting read()
    mem::IoBufChain reassembly_{}; // straddling post-handshake message bytes
    mem::IoBuf message_buf_{}; // materialized straddling message (owns the dispatched body)
    std::array<std::uint8_t, kOpenScratchSize> open_scratch_{};
    TlsSecret own_secret_; // 1.3 write-side rekey base; empty on 1.2
    TlsSecret peer_secret_; // 1.3 read-side rekey base; empty on 1.2
    std::array<std::uint8_t, 256> alpn_{};
    TlsProtocolVersion version_;
    TlsCipherSuiteId suite_;
    std::uint16_t alpn_len_ = 0;
    std::uint8_t warning_alerts_ = 0; // consecutive dropped warnings (see on_alert_bytes)
    bool rekey_pending_ = false; // peer's KeyUpdate(update_requested): owe a response before the next write
    bool peer_closed_ = false; // the peer's close_notify latched
    bool close_sent_ = false; // our close_notify encoded
    bool failed_ = false; // terminal latched (ours or the peer's)
};

TlsConnection::Impl::Impl(TlsConnectionRole role, TlsConnectedState &&state) noexcept :
    read_cipher_(std::move(state.read_cipher)), write_cipher_(std::move(state.write_cipher)),
    own_secret_(std::move(role == TlsConnectionRole::Client ? state.client_app_secret : state.server_app_secret)),
    peer_secret_(std::move(role == TlsConnectionRole::Client ? state.server_app_secret : state.client_app_secret)),
    version_(state.version), suite_(state.suite) {
    alpn_len_ = state.alpn_len;
    std::memcpy(alpn_.data(), state.alpn.data(), state.alpn_len);
    // Role picks which app secret is ours (write-side rekey base) — the
    // ciphers themselves are already endpoint-oriented by the engines' move.
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
    if (!fresh.init(suite_, TlsRecordProtectionKind::Tls13, TlsRecordDirection::Open, {keys->key.data(), keys->key_len},
                    {keys->iv.data(), keys->iv_len})
                 .has_value()) {
        latch_fatal(TlsAlertDesc::InternalError);
        return false;
    }
    peer_secret_ = std::move(*next);
    read_cipher_ = std::move(fresh);
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
    auto init = fresh.init(suite_, TlsRecordProtectionKind::Tls13, TlsRecordDirection::Seal,
                           {keys->key.data(), keys->key_len}, {keys->iv.data(), keys->iv_len});
    if (!init.has_value()) {
        return init;
    }
    // The response flies under the CURRENT write keys — the peer rotates on
    // receipt (RFC 8446 §4.6.1); our rotation applies to the records after.
    const std::uint8_t key_update[5] = {static_cast<std::uint8_t>(TlsHandshakeType::KeyUpdate), 0, 0, 1, 0};
    auto emitted = emit_sealed(TlsContentType::Handshake, key_update);
    if (!emitted.has_value()) {
        return emitted;
    }
    own_secret_ = std::move(*next);
    write_cipher_ = std::move(fresh);
    rekey_pending_ = false;
    return {};
}

void TlsConnection::Impl::latch_fatal(TlsAlertDesc alert) noexcept {
    if (failed_) {
        return;
    }
    failed_ = true;
    const std::uint8_t alert_bytes[] = {kAlertLevelFatal, static_cast<std::uint8_t>(alert)};
    (void) emit_sealed(TlsContentType::Alert, alert_bytes);
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

// ---- inbound record pipeline ----

void TlsConnection::Impl::route_record(TlsRecord &&record) noexcept {
    if (record.type != TlsContentType::Alert && record.type != TlsContentType::ApplicationData) {
        // The warning budget counts consecutive alerts only. A sealed record
        // (outer application_data) may be a 1.3 alert: open_and_route resets
        // on its decrypted inner type instead.
        warning_alerts_ = 0;
    }
    switch (record.type) {
        case TlsContentType::Alert: {
            if (version_ == TlsProtocolVersion::Tls13) {
                // 1.3: sealed alerts ride outer application_data (routed
                // below); an outer alert record is only ever the plaintext
                // 2-byte peer alert form — decoded as-is.
                if (record.length != 2) {
                    latch_fatal(TlsAlertDesc::DecodeError);
                    return;
                }
                std::array<std::uint8_t, 2> alert{};
                const std::uint8_t *bytes = record.contiguous_payload();
                if (bytes == nullptr) {
                    bytes = gather_small(record.payload, alert);
                }
                if (bytes == nullptr) {
                    latch_fatal(TlsAlertDesc::DecodeError);
                    return;
                }
                on_alert_bytes(bytes);
                return;
            }
            // 1.2: alerts after the CCS fly sealed (RFC 5246 §6.2.3.3) —
            // open first; the inner-alert branch of open_and_route decodes.
            open_and_route(record);
            return;
        }
        case TlsContentType::ChangeCipherSpec:
            // Post-handshake CCS has no legal meaning in either version:
            // 1.3's middlebox-compat window is handshake-only (RFC 8446
            // §5), and 1.2 renegotiation is out of scope (09 §1).
            latch_fatal(TlsAlertDesc::UnexpectedMessage);
            return;
        case TlsContentType::Handshake:
            if (version_ == TlsProtocolVersion::Tls13) {
                // Everything after the 1.3 ServerHello must be sealed.
                latch_fatal(TlsAlertDesc::UnexpectedMessage);
                return;
            }
            // 1.2 preserves the record type under encryption; the AAD-bound
            // inner type is this outer Handshake by construction.
            open_and_route(record);
            return;
        case TlsContentType::ApplicationData:
            open_and_route(record);
            return;
    }
    latch_fatal(TlsAlertDesc::UnexpectedMessage); // unreachable: the reader rejects unknown types
}

void TlsConnection::Impl::open_and_route(TlsRecord &record) noexcept {
    // Length bounds before any buffer arithmetic — no decrypt oracle, and
    // the chain adapter's body/tag sizing below stays underflow-free (the
    // cipher's own check would come too late for it on degenerate lengths).
    if (record.length < read_cipher_.min_ciphertext_size() || record.length > read_cipher_.max_ciphertext_size()) {
        latch_fatal(TlsAlertDesc::BadRecordMac);
        return;
    }
    const std::size_t dst_len = tls_record_open_dst_size(read_cipher_, record.length);
    FIBER_ASSERT(dst_len <= open_scratch_.size());
    const TlsRecordOpenChainResult result =
            tls_record_open_in_place(read_cipher_, record.type, record.legacy_version, record.length, record.payload,
                                     {open_scratch_.data(), dst_len});
    if (result.open.status != TlsRecordCipher::Status::Ok) {
        // Overflow is only reported after authentication, so naming it is no
        // oracle. AuthFail and pre-decryption Malformed both collapse to
        // bad_record_mac — no decrypt-oracle distinction is surfaced.
        latch_fatal(result.open.status == TlsRecordCipher::Status::Overflow ? TlsAlertDesc::RecordOverflow
                                                                            : TlsAlertDesc::BadRecordMac);
        return;
    }
    const std::uint8_t *plain = nullptr;
    if (result.in_chain) {
        const mem::IoBuf *front = record.payload.first_readable();
        if (front != nullptr && front->readable() == result.open.plain_len) {
            plain = front->readable_data();
        } else {
            // Defensive transcription of a non-contiguous in-chain plaintext
            // (not produced by the 05 eligibility rules).
            plain = gather_small(record.payload, {open_scratch_.data(), result.open.plain_len});
        }
    } else {
        plain = open_scratch_.data();
    }
    if (plain == nullptr) {
        latch_fatal(TlsAlertDesc::InternalError);
        return;
    }

    if (result.open.inner_type != TlsContentType::Alert) {
        warning_alerts_ = 0; // a non-alert record, by its inner type
    }
    switch (result.open.inner_type) {
        case TlsContentType::Alert:
            if (result.open.plain_len != 2) {
                latch_fatal(TlsAlertDesc::DecodeError);
                return;
            }
            on_alert_bytes(plain);
            return;
        case TlsContentType::ApplicationData:
            deliver_plaintext(record, result.in_chain, result.open.plain_len);
            return;
        case TlsContentType::Handshake:
            feed_handshake_fragment(plain, result.open.plain_len);
            return;
        default:
            // Inner CCS or app data outside any legal window: unexpected.
            latch_fatal(TlsAlertDesc::UnexpectedMessage);
            return;
    }
}

void TlsConnection::Impl::on_alert_bytes(const std::uint8_t *bytes) noexcept {
    // close_notify and the peer's fatal alert are the two terminals;
    // plaintext delivered before the alert stays readable. A 1.2 warning
    // (e.g. no_renegotiation) is dropped, up to kMaxWarningAlerts in a row
    // — the BoringSSL rule. 1.3 has no warning level; the one exception is
    // a warning user_canceled, which RFC 8446 §6.1 keeps and JDK 11 sends
    // before close_notify — dropped like a 1.2 warning (BoringSSL, NSS and
    // OpenSSL all skip it). Every other 1.3 alert stays terminal.
    const bool user_canceled = bytes[1] == static_cast<std::uint8_t>(TlsAlertDesc::UserCanceled);
    if (bytes[1] == static_cast<std::uint8_t>(TlsAlertDesc::CloseNotify)) {
        peer_closed_ = true;
    } else if (bytes[0] == kAlertLevelWarning && (version_ == TlsProtocolVersion::Tls12 || user_canceled)) {
        if (++warning_alerts_ > kMaxWarningAlerts) {
            latch_fatal(TlsAlertDesc::UnexpectedMessage);
        }
    } else {
        failed_ = true; // the peer's fatal alert: terminal, nothing to send
    }
}

void TlsConnection::Impl::deliver_plaintext(TlsRecord &record, bool in_chain, std::size_t plain_len) noexcept {
    if (plain_len == 0) {
        return; // empty inner app data (the 1.3 keep-alive form): nothing to deliver
    }
    if (in_chain) {
        // The in-place open already shrank the record chain's view to the
        // plaintext — the nodes move into the delivery chain, zero copies.
        FIBER_ASSERT(record.payload.readable_bytes() == plain_len);
        if (!plaintext_.append_chain(std::move(record.payload))) {
            latch_fatal(TlsAlertDesc::InternalError);
        }
        return;
    }
    mem::IoBuf fragment = mem::IoBuf::allocate(plain_len);
    if (!fragment.valid()) {
        latch_fatal(TlsAlertDesc::InternalError);
        return;
    }
    std::memcpy(fragment.writable_data(), open_scratch_.data(), plain_len);
    fragment.commit(plain_len);
    if (!plaintext_.append(std::move(fragment))) {
        latch_fatal(TlsAlertDesc::InternalError);
    }
}

void TlsConnection::Impl::feed_handshake_fragment(const std::uint8_t *frag, std::size_t len) noexcept {
    if (len == 0) {
        return; // empty handshake fragments carry no message bytes
    }
    std::size_t off = 0;
    for (;;) {
        if (reassembly_.readable_bytes() > 0) {
            // A message is already accumulating: top the chain up with the
            // fragment tail, then try to complete from the chain.
            if (off < len && !append_fragment({frag + off, len - off})) {
                latch_fatal(TlsAlertDesc::InternalError);
                return;
            }
            off = len;
            const std::size_t total = reassembly_.readable_bytes();
            std::array<std::uint8_t, kTlsHandshakeHeaderSize> header{};
            if (gather_small(reassembly_, header) == nullptr) {
                latch_fatal(TlsAlertDesc::InternalError);
                return;
            }
            const std::size_t body_len = (static_cast<std::size_t>(header[1]) << 16) |
                                         (static_cast<std::size_t>(header[2]) << 8) |
                                         static_cast<std::size_t>(header[3]);
            if (body_len > kMaxPostHandshakeMessage) {
                latch_fatal(TlsAlertDesc::DecodeError);
                return;
            }
            const std::size_t message_len = kTlsHandshakeHeaderSize + body_len;
            if (total < message_len) {
                return;
            }
            if (!materialize_message(message_len)) {
                latch_fatal(TlsAlertDesc::InternalError);
                return;
            }
            if (!dispatch_post_handshake(static_cast<TlsHandshakeType>(header[0]),
                                         {message_buf_.readable_data() + kTlsHandshakeHeaderSize, body_len})) {
                return;
            }
            continue; // the chain may hold more complete messages
        }

        // Fast path: the whole message inside the fragment (borrowed span —
        // the record's own bytes or the scratch, both alive for the call).
        const std::size_t avail = len - off;
        if (avail == 0) {
            return; // fragment fully consumed by whole messages
        }
        if (avail < kTlsHandshakeHeaderSize) {
            if (!append_fragment({frag + off, avail})) {
                latch_fatal(TlsAlertDesc::InternalError);
            }
            return;
        }
        const std::size_t body_len = (static_cast<std::size_t>(frag[off + 1]) << 16) |
                                     (static_cast<std::size_t>(frag[off + 2]) << 8) |
                                     static_cast<std::size_t>(frag[off + 3]);
        if (body_len > kMaxPostHandshakeMessage) {
            latch_fatal(TlsAlertDesc::DecodeError);
            return;
        }
        if (off + kTlsHandshakeHeaderSize + body_len <= len) {
            if (!dispatch_post_handshake(static_cast<TlsHandshakeType>(frag[off]),
                                         {frag + off + kTlsHandshakeHeaderSize, body_len})) {
                return;
            }
            off += kTlsHandshakeHeaderSize + body_len;
            continue;
        }
        if (!append_fragment({frag + off, len - off})) {
            latch_fatal(TlsAlertDesc::InternalError);
        }
        return;
    }
}

bool TlsConnection::Impl::append_fragment(std::span<const std::uint8_t> bytes) noexcept {
    mem::IoBuf fragment = mem::IoBuf::allocate(bytes.size());
    if (!fragment.valid()) {
        return false;
    }
    std::memcpy(fragment.writable_data(), bytes.data(), bytes.size());
    fragment.commit(bytes.size());
    return reassembly_.append(std::move(fragment));
}

bool TlsConnection::Impl::materialize_message(std::size_t message_len) noexcept {
    // Copy the complete message out of the chain (the one materializing
    // copy for straddling messages) and consume what it held.
    message_buf_ = mem::IoBuf::allocate(message_len);
    if (!message_buf_.valid()) {
        return false;
    }
    std::size_t done = 0;
    while (done < message_len) {
        mem::IoBuf *front = reassembly_.first_readable();
        if (front == nullptr) {
            return false;
        }
        const std::size_t take = std::min(front->readable(), message_len - done);
        std::memcpy(message_buf_.writable_data() + done, front->readable_data(), take);
        done += take;
        reassembly_.consume(take);
    }
    message_buf_.commit(message_len);
    return true;
}

common::IoResult<void> TlsConnection::Impl::emit_sealed(TlsContentType type,
                                                        std::span<const std::uint8_t> payload) noexcept {
    std::size_t off = 0;
    do {
        const std::size_t chunk = std::min(payload.size() - off, kTlsMaxPlaintextSize);
        const std::size_t sealed_len = write_cipher_.seal_output_size(chunk);
        mem::IoBuf record_buf = mem::IoBuf::allocate(kTlsRecordHeaderSize + sealed_len);
        if (!record_buf.valid()) {
            return std::unexpected(common::IoErr::NoMem);
        }
        tls_encode_record_header(record_buf.writable_data(), outer_record_type(type), kTlsRecordVersionTls12,
                                 static_cast<std::uint16_t>(sealed_len));
        const TlsRecordCipher::SealResult sealed = write_cipher_.seal(
                type, {payload.data() + off, chunk}, {record_buf.writable_data() + kTlsRecordHeaderSize, sealed_len});
        if (sealed.status != TlsRecordCipher::Status::Ok) {
            return std::unexpected(common::IoErr::Invalid);
        }
        record_buf.commit(kTlsRecordHeaderSize + sealed.out_len);
        if (!out_.append(std::move(record_buf))) {
            return std::unexpected(common::IoErr::NoMem);
        }
        off += chunk;
    } while (off < payload.size());
    return {};
}

// =====================================================================
// public shell
// =====================================================================

TlsConnection::TlsConnection(TlsConnectionRole role, TlsConnectedState &&state) noexcept :
    impl_(new (std::nothrow) Impl(role, std::move(state))) {}

TlsConnection::~TlsConnection() { delete impl_; }

void TlsConnection::on_record(TlsRecord &&record) noexcept {
    if (impl_ == nullptr || impl_->failed_ || impl_->peer_closed_) {
        return; // terminal: records drop
    }
    impl_->route_record(std::move(record));
}

void TlsConnection::on_framing_fatal(TlsAlertDesc alert) noexcept {
    if (impl_ == nullptr) {
        return;
    }
    impl_->latch_fatal(alert);
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

TlsConnection::ReadStatus TlsConnection::take(std::size_t size, mem::IoBufChain &out, std::size_t &out_len) noexcept {
    out_len = 0;
    if (impl_ == nullptr) {
        return ReadStatus::Fatal;
    }
    const std::size_t available = impl_->plaintext_.readable_bytes();
    if (size == 0 || available == 0) {
        if (impl_->failed_) {
            return ReadStatus::Fatal;
        }
        if (impl_->peer_closed_) {
            return ReadStatus::PeerClosed;
        }
        return ReadStatus::NeedMore;
    }
    const std::size_t bytes = size < available ? size : available;
    if (!impl_->plaintext_.take_prefix(bytes, out)) {
        return ReadStatus::NoMem; // rolled back — the plaintext is untouched
    }
    out_len = bytes;
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
    return impl_->emit_sealed(TlsContentType::ApplicationData, payload);
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
    const auto emitted = impl_->emit_sealed(TlsContentType::Alert, alert);
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
    return std::move(impl_->out_);
}

bool TlsConnection::peer_closed() const noexcept { return impl_ != nullptr && impl_->peer_closed_; }

bool TlsConnection::failed() const noexcept { return impl_ == nullptr || impl_->failed_; }

std::span<const std::uint8_t> TlsConnection::alpn() const noexcept {
    return impl_ == nullptr ? std::span<const std::uint8_t>{}
                            : std::span<const std::uint8_t>{impl_->alpn_.data(), impl_->alpn_len_};
}

} // namespace fiber::tls
