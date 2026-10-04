#include "TlsHandshakeContext.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include <fiber/common/Assert.h>
#include <fiber/tls/record/TlsRecordCipherChain.h>

namespace fiber::tls {

namespace {

TlsInboundStep step_need_more() noexcept {
    TlsInboundStep step;
    step.kind = TlsInboundStep::Kind::NeedMore;
    return step;
}

TlsInboundStep step_fatal(TlsAlertDesc alert) noexcept {
    TlsInboundStep step;
    step.kind = TlsInboundStep::Kind::Fatal;
    step.alert = alert;
    return step;
}

constexpr std::uint8_t kAlertLevelFatal = 2;

// Gathers `out.size()` bytes off a chain's readable prefix into `out`
// (records fed one byte at a time can straddle nodes); nullptr when the chain
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

TlsHandshakeContext::TlsHandshakeContext() noexcept = default;

TlsHandshakeContext::~TlsHandshakeContext() noexcept = default;

bool TlsHandshakeContext::feed(mem::IoBuf &&bytes) noexcept {
    FIBER_ASSERT(quic_ == nullptr); // QUIC mode feeds via provide_quic
    return reader_.feed(std::move(bytes));
}

bool TlsHandshakeContext::feed(mem::IoBufChain &&bytes) noexcept {
    FIBER_ASSERT(quic_ == nullptr); // QUIC mode feeds via provide_quic
    return reader_.feed(std::move(bytes));
}

void TlsHandshakeContext::enable_quic(const TlsQuicCallbacks &callbacks) noexcept {
    FIBER_ASSERT(quic_ == nullptr && callbacks.set_secret != nullptr && callbacks.add_handshake_data != nullptr &&
                 callbacks.on_peer_transport_params != nullptr && callbacks.send_alert != nullptr);
    quic_ = &callbacks;
}

void TlsHandshakeContext::set_quic_level(TlsQuicLevel level) noexcept {
    FIBER_ASSERT(static_cast<std::uint8_t>(level) >= static_cast<std::uint8_t>(quic_level_));
    quic_level_ = level;
}

bool TlsHandshakeContext::provide_quic(TlsQuicLevel level, mem::IoBufChain &bytes) noexcept {
    FIBER_ASSERT(quic_ != nullptr && static_cast<std::uint8_t>(level) >=
                                             static_cast<std::uint8_t>(quic_provided_level_)); // RFC 9001 §4.1.3 gate
    quic_provided_level_ = level;
    FIBER_ASSERT(!bytes.complete());
    return reassembly_.append_chain(std::move(bytes));
}

mem::IoBufChain TlsHandshakeContext::take_output() noexcept { return std::move(out_); }

std::size_t TlsHandshakeContext::pending_bytes() const noexcept {
    return reader_.pending_bytes() + reassembly_.readable_bytes() + (has_current_ ? plain_len_ - current_off_ : 0);
}

// ---- inbound ----

TlsInboundStep TlsHandshakeContext::step() noexcept {
    if (quic_ != nullptr) {
        return quic_step();
    }
    for (;;) {
        if (has_current_ && current_off_ < plain_len_) {
            const TlsInboundStep message = extract_message();
            if (message.kind != TlsInboundStep::Kind::NeedMore) {
                return message;
            }
        }
        if (has_current_) {
            current_ = TlsRecord{};
            has_current_ = false;
            plain_ = nullptr;
            plain_len_ = 0;
            current_off_ = 0;
        }
        TlsRecordReader::Result next = reader_.next();
        if (next.status == TlsRecordReader::Result::Status::Fatal) {
            return step_fatal(next.alert);
        }
        if (next.status == TlsRecordReader::Result::Status::NeedMore) {
            return step_need_more();
        }
        const TlsInboundStep routed = take_record(std::move(next.record));
        if (routed.kind != TlsInboundStep::Kind::NeedMore) {
            return routed;
        }
        // Routed into the reassembler without a complete message yet: pull
        // the next record and keep accumulating.
    }
}

// Warning-level alerts other than close_notify are dropped until TLS 1.3 is
// settled — the BoringSSL rule (ssl_process_alert): misconfigured 1.2 servers
// send unrecognized_name as a warning, OpenSSL even before its ServerHello,
// while our client cannot yet know the version. Once 1.3 is settled only a
// warning user_canceled is still dropped: RFC 8446 §6.1 keeps it without
// saying how to handle it, and JDK 11 sends it before close_notify
// (BoringSSL, NSS and OpenSSL all skip it). More than kMaxWarningAlerts in
// a row is unexpected_message. Every other alert (fatal, close_notify, any
// other 1.3 warning) is surfaced and terminal.
TlsInboundStep TlsHandshakeContext::alert_step(const std::uint8_t *bytes) noexcept {
    const bool close_notify = bytes[1] == static_cast<std::uint8_t>(TlsAlertDesc::CloseNotify);
    const bool tls13 = mode_ == TlsInboundMode::Sealed13 || tls13_settled_;
    if (bytes[0] == static_cast<std::uint8_t>(TlsAlertLevel::Warning) && !close_notify &&
        (!tls13 || bytes[1] == static_cast<std::uint8_t>(TlsAlertDesc::UserCanceled))) {
        if (++warning_alerts_ > kMaxWarningAlerts) {
            return step_fatal(TlsAlertDesc::UnexpectedMessage);
        }
        return step_need_more();
    }
    TlsInboundStep step;
    step.kind = TlsInboundStep::Kind::Alert;
    step.alert = static_cast<TlsAlertDesc>(bytes[1]);
    step.close_notify = close_notify;
    return step;
}

TlsInboundStep TlsHandshakeContext::take_record(TlsRecord &&record) noexcept {
    if (record.type != TlsContentType::Alert && record.type != TlsContentType::ApplicationData) {
        // The warning budget counts consecutive alerts only. A sealed record
        // (outer application_data) may be a 1.3 alert: open_current resets
        // on its decrypted inner type instead.
        warning_alerts_ = 0;
    }
    switch (record.type) {
        case TlsContentType::Alert: {
            if (mode_ == TlsInboundMode::Sealed12) {
                // 1.2: alerts after the CCS fly sealed (RFC 5246 §6.2.3.3) —
                // open first; the inner-alert branch of open_current decodes
                // and routes. A plaintext alert there is unreachable by
                // construction (the outer type under 1.2 encryption IS the
                // AAD-bound inner type).
                return open_current(record);
            }
            if (record.length != 2) {
                return step_fatal(TlsAlertDesc::DecodeError);
            }
            std::array<std::uint8_t, 2> alert{};
            const std::uint8_t *bytes = record.contiguous_payload();
            if (bytes == nullptr) {
                bytes = gather_small(record.payload, alert);
            }
            if (bytes == nullptr) {
                return step_fatal(TlsAlertDesc::DecodeError);
            }
            return alert_step(bytes);
        }
        case TlsContentType::ChangeCipherSpec: {
            // RFC 8446 §5: any CCS value other than a single 0x01 aborts the
            // handshake — enforced here for both versions; the semantics (1.3
            // ignore vs 1.2 read-cipher switch) are the engine's.
            if (mode_ == TlsInboundMode::Sealed12 || record.length != 1) {
                return step_fatal(TlsAlertDesc::UnexpectedMessage);
            }
            std::array<std::uint8_t, 1> ccs{};
            const std::uint8_t *bytes = record.contiguous_payload();
            if (bytes == nullptr) {
                bytes = gather_small(record.payload, ccs);
            }
            if (bytes == nullptr || bytes[0] != 1) {
                return step_fatal(TlsAlertDesc::UnexpectedMessage);
            }
            TlsInboundStep step;
            step.kind = TlsInboundStep::Kind::Ccs;
            return step;
        }
        case TlsContentType::Handshake: {
            if (mode_ == TlsInboundMode::Sealed13) {
                // 06 §2.6: everything after the 1.3 ServerHello must be sealed.
                return step_fatal(TlsAlertDesc::UnexpectedMessage);
            }
            if (mode_ == TlsInboundMode::Plaintext13 && record.length > 0 &&
                ++plaintext_records_13_ > kMaxPlaintextHandshakeRecords13) {
                return step_fatal(TlsAlertDesc::UnexpectedMessage);
            }
            if (mode_ == TlsInboundMode::Plaintext13 && early_skip_armed_) {
                // Post-HRR early-data skip, type variant: the first plaintext
                // handshake record is CH2 — the skip window ends with it.
                early_skip_armed_ = false;
            }
            if (mode_ == TlsInboundMode::Sealed12) {
                // 1.2 preserves the record type under encryption; the AAD-bound
                // inner type is this outer Handshake by construction.
                const TlsInboundStep opened = open_current(record);
                if (opened.kind != TlsInboundStep::Kind::NeedMore) {
                    return opened;
                }
                return extract_message();
            }
            return route_current(std::move(record));
        }
        case TlsContentType::ApplicationData: {
            if (mode_ == TlsInboundMode::Plaintext13 && early_skip_armed_) {
                // Post-HRR early-data skip (RFC 8446 §4.2.10): undecryptable
                // in-flight 0-RTT records are discarded by type up to the
                // budget while CH2 is owed.
                early_used_ += record.length;
                if (early_used_ > early_budget_) {
                    return step_fatal(TlsAlertDesc::UnexpectedMessage);
                }
                return step_need_more();
            }
            if (mode_ != TlsInboundMode::Sealed13 && mode_ != TlsInboundMode::Sealed12) {
                // Unencrypted app data is never legitimate inside a handshake.
                return step_fatal(TlsAlertDesc::UnexpectedMessage);
            }
            return open_current(record);
        }
    }
    return step_fatal(TlsAlertDesc::UnexpectedMessage);
}

// Decrypts `record` in place and adopts it as the current fragment (plain_
// into the record's own bytes, or open_scratch_ when the chain topology
// forced a transcription). Surfaces non-handshake inner outcomes; NeedMore
// means "handshake plaintext installed — caller may extract".
TlsInboundStep TlsHandshakeContext::open_current(TlsRecord &record) noexcept {
    // Length bounds before any buffer arithmetic (as TlsConnection does): a
    // sealed 1.2 GCM record shorter than its explicit nonce would otherwise
    // size the open workspace from a wrapped subtraction and abort on the
    // scratch-capacity assert — a remote crash for any peer past its CCS.
    // Out-of-range lengths are framing faults, fatal even in the rejected-
    // 0-RTT skip window (which only forgives authentication failures).
    if (record.length < read_cipher_.min_ciphertext_size() || record.length > read_cipher_.max_ciphertext_size()) {
        return step_fatal(TlsAlertDesc::BadRecordMac);
    }
    const std::size_t dst_len = tls_record_open_dst_size(read_cipher_, record.length);
    FIBER_ASSERT(dst_len <= open_scratch_.size());
    const TlsRecordOpenChainResult result =
            tls_record_open_in_place(read_cipher_, record.type, record.legacy_version, record.length, record.payload,
                                     {open_scratch_.data(), dst_len});
    if (result.open.status != TlsRecordCipher::Status::Ok) {
        if (early_skip_armed_ && result.open.status == TlsRecordCipher::Status::AuthFail) {
            // Rejected-0-RTT trial open (RFC 8446 §4.2.10): this record was
            // sealed under early keys we never derived — discard it up to the
            // ciphertext budget and keep pumping for the client's second
            // flight. Malformed framing stays fatal.
            early_used_ += record.length;
            if (early_used_ > early_budget_) {
                return step_fatal(TlsAlertDesc::UnexpectedMessage);
            }
            return step_need_more();
        }
        if (result.open.status == TlsRecordCipher::Status::Overflow) {
            return step_fatal(TlsAlertDesc::RecordOverflow); // authenticated: no oracle
        }
        // AuthFail and pre-decryption Malformed both collapse to
        // bad_record_mac — no decrypt-oracle distinction is surfaced.
        return step_fatal(TlsAlertDesc::BadRecordMac);
    }
    if (early_skip_armed_) {
        // The first record that opens under the handshake key IS the client's
        // second flight (Fin or its first sealed message).
        early_skip_armed_ = false;
    }
    current_ = std::move(record);
    has_current_ = true;
    current_off_ = 0;
    plain_len_ = result.open.plain_len;
    plain_ = nullptr;
    if (result.in_chain) {
        const mem::IoBuf *front = current_.payload.first_readable();
        if (front != nullptr && front->readable() == plain_len_) {
            plain_ = front->readable_data();
        }
    }
    if (plain_ == nullptr) {
        // Not in-chain contiguous: the plaintext is (or lands) in the scratch.
        if (result.in_chain) {
            // Defensive transcription of a non-contiguous in-chain plaintext
            // (not produced by the 05 eligibility rules).
            plain_ = gather_small(current_.payload, {open_scratch_.data(), plain_len_});
            if (plain_ == nullptr) {
                return step_fatal(TlsAlertDesc::InternalError);
            }
        } else {
            plain_ = open_scratch_.data();
        }
    }

    if (result.open.inner_type != TlsContentType::Alert) {
        warning_alerts_ = 0; // a non-alert record, by its inner type
    }
    if (result.open.inner_type == TlsContentType::Alert) {
        if (plain_len_ != 2) {
            return step_fatal(TlsAlertDesc::DecodeError);
        }
        current_off_ = plain_len_; // consumed
        return alert_step(plain_);
    }
    if (result.open.inner_type == TlsContentType::ApplicationData && early_sink_armed_) {
        // Accepted-0-RTT window: the content bytes (the inner type byte and
        // padding are already stripped) are 0-RTT application data — budget,
        // copy into the sink, and keep pumping records.
        early_used_ += plain_len_;
        if (early_used_ > early_budget_) {
            return step_fatal(TlsAlertDesc::UnexpectedMessage);
        }
        if (plain_len_ > 0) {
            mem::IoBuf fragment = mem::IoBuf::allocate(plain_len_);
            if (!fragment.valid()) {
                return step_fatal(TlsAlertDesc::InternalError);
            }
            std::memcpy(fragment.writable_data(), plain_, plain_len_);
            fragment.commit(plain_len_);
            if (!early_sink_->append(std::move(fragment))) {
                return step_fatal(TlsAlertDesc::InternalError);
            }
        }
        current_off_ = plain_len_; // consumed as early data
        return step_need_more();
    }
    if (result.open.inner_type != TlsContentType::Handshake) {
        // Inner CCS or app data outside the early window: unexpected (06 §2.6).
        return step_fatal(TlsAlertDesc::UnexpectedMessage);
    }
    return step_need_more();
}

// Adopts a plaintext (never sealed) Handshake record as the current fragment
// and tries to complete a message from it.
TlsInboundStep TlsHandshakeContext::route_current(TlsRecord &&record) noexcept {
    if (record.length == 0) {
        // Empty handshake fragments are tolerated and carry no message bytes.
        return step_need_more();
    }
    const std::uint8_t *contiguous = record.contiguous_payload();
    current_ = std::move(record); // chain move steals pointers; storage stays put
    has_current_ = true;
    plain_len_ = current_.length;
    current_off_ = 0;
    if (contiguous != nullptr) {
        plain_ = contiguous;
    } else {
        // Straddling plaintext: transcribe into the scratch — the one copy
        // the codec's contiguous-body requirement costs (06 §5.1).
        plain_ = gather_small(current_.payload, {open_scratch_.data(), current_.length});
        if (plain_ == nullptr) {
            return step_fatal(TlsAlertDesc::InternalError);
        }
    }
    return extract_message();
}

std::size_t TlsHandshakeContext::max_body_len(std::uint8_t type) const noexcept {
    return type == static_cast<std::uint8_t>(TlsHandshakeType::Certificate) ? kMaxCertificateMessage : max_message_;
}

bool TlsHandshakeContext::append_fragment(std::span<const std::uint8_t> bytes) noexcept {
    mem::IoBuf fragment = mem::IoBuf::allocate(bytes.size());
    if (!fragment.valid()) {
        return false;
    }
    std::memcpy(fragment.writable_data(), bytes.data(), bytes.size());
    fragment.commit(bytes.size());
    return reassembly_.append(std::move(fragment));
}

TlsInboundStep TlsHandshakeContext::extract_message() noexcept {
    // A message is already accumulating: top the reassembly chain up with the
    // current fragment, then try to complete from the chain.
    if (!reassembly_.empty()) {
        if (current_off_ < plain_len_) {
            if (!append_fragment({plain_ + current_off_, plain_len_ - current_off_})) {
                return step_fatal(TlsAlertDesc::InternalError);
            }
            current_off_ = plain_len_;
        }
        const std::size_t total = reassembly_.readable_bytes();
        if (total < kTlsHandshakeHeaderSize) {
            return step_need_more();
        }
        std::array<std::uint8_t, kTlsHandshakeHeaderSize> header{};
        if (gather_small(reassembly_, header) == nullptr) {
            return step_fatal(TlsAlertDesc::InternalError);
        }
        const std::size_t body_len = (static_cast<std::size_t>(header[1]) << 16) |
                                     (static_cast<std::size_t>(header[2]) << 8) | static_cast<std::size_t>(header[3]);
        if (body_len > max_body_len(header[0])) {
            return step_fatal(TlsAlertDesc::DecodeError);
        }
        const std::size_t message_len = kTlsHandshakeHeaderSize + body_len;
        if (total < message_len) {
            return step_need_more();
        }
        message_buf_ = mem::IoBuf::allocate(message_len);
        if (!message_buf_.valid()) {
            return step_fatal(TlsAlertDesc::InternalError);
        }
        // Copy the complete message out of the chain (the one materializing
        // copy for straddling messages) and consume what it held.
        std::size_t done = 0;
        while (done < message_len) {
            mem::IoBuf *front = reassembly_.first_readable();
            if (front == nullptr) {
                return step_fatal(TlsAlertDesc::InternalError);
            }
            const std::size_t take = std::min(front->readable(), message_len - done);
            std::memcpy(message_buf_.writable_data() + done, front->readable_data(), take);
            done += take;
            reassembly_.consume(take);
        }
        message_buf_.commit(message_len);
        plain_ = message_buf_.readable_data();
        plain_len_ = message_len;
        current_off_ = message_len; // fully consumed; span owned by message_buf_
        TlsInboundStep step;
        step.kind = TlsInboundStep::Kind::Message;
        step.type = static_cast<TlsHandshakeType>(header[0]);
        step.body = {plain_ + kTlsHandshakeHeaderSize, body_len};
        return step;
    }

    // Fast path: the whole message inside the current contiguous fragment.
    if (plain_len_ - current_off_ < kTlsHandshakeHeaderSize) {
        if (!append_fragment({plain_ + current_off_, plain_len_ - current_off_})) {
            return step_fatal(TlsAlertDesc::InternalError);
        }
        current_off_ = plain_len_;
        return step_need_more();
    }
    const std::size_t off = current_off_;
    const std::size_t body_len = (static_cast<std::size_t>(plain_[off + 1]) << 16) |
                                 (static_cast<std::size_t>(plain_[off + 2]) << 8) |
                                 static_cast<std::size_t>(plain_[off + 3]);
    if (body_len > max_body_len(plain_[off])) {
        return step_fatal(TlsAlertDesc::DecodeError);
    }
    if (off + kTlsHandshakeHeaderSize + body_len <= plain_len_) {
        current_off_ = off + kTlsHandshakeHeaderSize + body_len;
        TlsInboundStep step;
        step.kind = TlsInboundStep::Kind::Message;
        step.type = static_cast<TlsHandshakeType>(plain_[off]);
        step.body = {plain_ + off + kTlsHandshakeHeaderSize, body_len};
        return step;
    }
    if (!append_fragment({plain_ + off, plain_len_ - off})) {
        return step_fatal(TlsAlertDesc::InternalError);
    }
    current_off_ = plain_len_;
    return step_need_more();
}

// QUIC inbound (10 §3.3): the CRYPTO stream is raw handshake messages — the
// same 4-byte-header reassembly contract as the record path, minus records,
// alerts, CCS, and the 1.3 plaintext-record DOs bound (a CH may arrive in
// any fragmentation; the per-type message bound below is the surviving
// limit). Contiguous messages retain their storage in message_buf_; only
// straddling messages are copied. The body span stays valid until the next step().
TlsInboundStep TlsHandshakeContext::quic_step() noexcept {
    message_buf_ = mem::IoBuf{}; // the previous borrowed body expires at this step
    const std::size_t total = reassembly_.readable_bytes();
    if (total < kTlsHandshakeHeaderSize) {
        return step_need_more();
    }
    std::array<std::uint8_t, kTlsHandshakeHeaderSize> header{};
    if (gather_small(reassembly_, header) == nullptr) {
        return step_fatal(TlsAlertDesc::InternalError);
    }
    const std::size_t body_len = (static_cast<std::size_t>(header[1]) << 16) |
                                 (static_cast<std::size_t>(header[2]) << 8) | static_cast<std::size_t>(header[3]);
    if (body_len > max_body_len(header[0])) {
        return step_fatal(TlsAlertDesc::DecodeError);
    }
    const std::size_t message_len = kTlsHandshakeHeaderSize + body_len;
    if (total < message_len) {
        return step_need_more();
    }
    mem::IoBuf *front = reassembly_.first_readable();
    FIBER_ASSERT(front != nullptr);
    if (front->readable() >= message_len) {
        message_buf_ = front->retain_slice(0, message_len);
        reassembly_.consume_and_compact(message_len);
    } else {
        message_buf_ = mem::IoBuf::allocate(message_len);
        if (!message_buf_.valid()) {
            return step_fatal(TlsAlertDesc::InternalError);
        }
        std::size_t done = 0;
        while (done < message_len) {
            front = reassembly_.first_readable();
            FIBER_ASSERT(front != nullptr);
            const std::size_t take = std::min(front->readable(), message_len - done);
            std::memcpy(message_buf_.writable_data() + done, front->readable_data(), take);
            done += take;
            reassembly_.consume_and_compact(take);
        }
        message_buf_.commit(message_len);
    }
    TlsInboundStep step;
    step.kind = TlsInboundStep::Kind::Message;
    step.type = static_cast<TlsHandshakeType>(header[0]);
    step.body = {message_buf_.readable_data() + kTlsHandshakeHeaderSize, body_len};
    return step;
}

// ---- outbound ----

common::IoResult<void> TlsHandshakeContext::emit(TlsContentType type, std::span<const std::uint8_t> payload,
                                                 TlsRecordCipher *cipher) noexcept {
    if (quic_ != nullptr) {
        // QUIC: the payload is one fully-encoded handshake message; the QUIC
        // layer frames it as CRYPTO at the current outbound level. Records
        // never exist — type and any cipher argument are TCP concepts.
        return quic_->add_handshake_data(quic_->ctx, quic_level_, payload) ? common::IoResult<void>{}
                                                                           : std::unexpected(common::IoErr::NoMem);
    }
    TlsRecordCipher *seal_with = nullptr;
    if (cipher != nullptr) {
        FIBER_ASSERT(cipher->initialized());
        seal_with = cipher;
    } else if (write_cipher_.initialized()) {
        seal_with = &write_cipher_;
    }

    if (seal_with == nullptr) {
        // Plaintext: stage the exact payload once; the Writer splices it with
        // zero framing copies (one header IoBuf per record).
        mem::IoBuf staged = mem::IoBuf::allocate(payload.size());
        if (!staged.valid()) {
            return std::unexpected(common::IoErr::NoMem);
        }
        std::memcpy(staged.writable_data(), payload.data(), payload.size());
        staged.commit(payload.size());
        return writer_.write(type, std::move(staged), out_);
    }

    // Sealed: one record per <=14KiB chunk. 1.3 rewrites the outer type to
    // application_data; 1.2 preserves the payload's true content type under
    // encryption (the AAD binds it either way). Both fly at 0x0303.
    const TlsContentType outer_type =
            seal_with->kind() == TlsRecordProtectionKind::Tls13 ? TlsContentType::ApplicationData : type;
    std::size_t off = 0;
    do {
        const std::size_t chunk = std::min(payload.size() - off, kTlsMaxPlaintextSize);
        const std::size_t sealed_len = seal_with->seal_output_size(chunk);
        mem::IoBuf record_buf = mem::IoBuf::allocate(kTlsRecordHeaderSize + sealed_len);
        if (!record_buf.valid()) {
            return std::unexpected(common::IoErr::NoMem);
        }
        tls_encode_record_header(record_buf.writable_data(), outer_type, kTlsRecordVersionTls12,
                                 static_cast<std::uint16_t>(sealed_len));
        const TlsRecordCipher::SealResult sealed = seal_with->seal(
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

common::IoResult<void> TlsHandshakeContext::send_ccs() noexcept {
    // QUIC: the compat CCS is forbidden (RFC 9001 §5.3) — a successful
    // no-op keeps the sub-flow call sites mode-blind.
    if (quic_ != nullptr) {
        return {};
    }
    // Always plaintext, independent of the write-cipher state (a CCS by
    // definition precedes the cipher it announces).
    mem::IoBuf staged = mem::IoBuf::allocate(1);
    if (!staged.valid()) {
        return std::unexpected(common::IoErr::NoMem);
    }
    staged.writable_data()[0] = 0x01;
    staged.commit(1);
    return writer_.write(TlsContentType::ChangeCipherSpec, std::move(staged), out_);
}

void TlsHandshakeContext::fail(TlsAlertDesc desc) noexcept {
    if (failed_) {
        return;
    }
    failed_ = true;
    failure_alert_ = desc;
    if (quic_ != nullptr) {
        // QUIC: nothing is encoded — the layer translates the alert into a
        // CONNECTION_CLOSE 0x0100|desc (10 定谳 2).
        quic_->send_alert(quic_->ctx, desc);
        return;
    }
    const std::uint8_t alert[] = {kAlertLevelFatal, static_cast<std::uint8_t>(desc)};
    (void) emit(TlsContentType::Alert, alert, nullptr);
}

} // namespace fiber::tls
