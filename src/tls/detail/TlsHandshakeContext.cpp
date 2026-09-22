#include "TlsHandshakeContext.h"

#include <algorithm>
#include <cstring>
#include <utility>

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

void TlsHandshakeContext::bind(mem::IoBufNodePool &pool) noexcept {
    reader_.bind_node_pool(pool);
    writer_.bind_node_pool(pool);
    out_ = mem::IoBufChain(pool);
    reassembly_ = mem::IoBufChain(pool);
}

bool TlsHandshakeContext::feed(mem::IoBuf &&bytes) noexcept { return reader_.feed(std::move(bytes)); }

bool TlsHandshakeContext::feed(mem::IoBufChain &&bytes) noexcept { return reader_.feed(std::move(bytes)); }

mem::IoBufChain TlsHandshakeContext::take_output() noexcept {
    mem::IoBufNodePool *pool = out_.bound() ? &out_.node_pool() : nullptr;
    mem::IoBufChain out = std::move(out_);
    if (pool != nullptr) {
        out_ = mem::IoBufChain(*pool);
    }
    return out;
}

std::size_t TlsHandshakeContext::pending_bytes() const noexcept {
    return reader_.pending_bytes() + reassembly_.readable_bytes() + (has_current_ ? plain_len_ - current_off_ : 0);
}

// ---- inbound ----

TlsInboundStep TlsHandshakeContext::step() noexcept {
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

TlsInboundStep TlsHandshakeContext::take_record(TlsRecord &&record) noexcept {
    switch (record.type) {
        case TlsContentType::Alert: {
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
            TlsInboundStep step;
            step.kind = TlsInboundStep::Kind::Alert;
            step.alert = static_cast<TlsAlertDesc>(bytes[1]);
            step.close_notify = bytes[1] == static_cast<std::uint8_t>(TlsAlertDesc::CloseNotify);
            return step; // severity byte ignored: mid-handshake every inbound alert
                         // is terminal (06 §2.3 for 1.3; 1.2 has no benign warning
                         // inside the initial handshake either)
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
    const std::size_t dst_len = tls_record_open_dst_size(read_cipher_, record.length);
    FIBER_ASSERT(dst_len <= open_scratch_.size());
    const TlsRecordOpenChainResult result =
            tls_record_open_in_place(read_cipher_, record.type, record.legacy_version, record.length, record.payload,
                                     {open_scratch_.data(), dst_len});
    if (result.open.status != TlsRecordCipher::Status::Ok) {
        // AuthFail and pre-decryption Malformed both collapse to
        // bad_record_mac — no decrypt-oracle distinction is surfaced.
        return step_fatal(TlsAlertDesc::BadRecordMac);
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

    if (result.open.inner_type == TlsContentType::Alert) {
        if (plain_len_ != 2) {
            return step_fatal(TlsAlertDesc::DecodeError);
        }
        current_off_ = plain_len_; // consumed
        TlsInboundStep step;
        step.kind = TlsInboundStep::Kind::Alert;
        step.alert = static_cast<TlsAlertDesc>(plain_[1]);
        step.close_notify = plain_[1] == static_cast<std::uint8_t>(TlsAlertDesc::CloseNotify);
        return step;
    }
    if (result.open.inner_type != TlsContentType::Handshake) {
        // Inner CCS or app data inside a handshake record: unexpected (06 §2.6).
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
        if (body_len > kMaxReassembledMessage) {
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
    if (body_len > kMaxReassembledMessage) {
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

// ---- outbound ----

common::IoResult<void> TlsHandshakeContext::emit(TlsContentType type, std::span<const std::uint8_t> payload,
                                                 TlsRecordCipher *cipher) noexcept {
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
    const std::uint8_t alert[] = {kAlertLevelFatal, static_cast<std::uint8_t>(desc)};
    (void) emit(TlsContentType::Alert, alert, nullptr);
}

} // namespace fiber::tls
