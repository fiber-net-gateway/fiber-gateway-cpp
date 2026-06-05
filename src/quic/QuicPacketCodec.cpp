#include "QuicPacketCodec.h"

#include <algorithm>
#include <array>
#include <expected>

#include "QuicCrypto.h"
#include "QuicCursor.h"
#include "QuicTransportCodec.h"

namespace fiber::quic {

namespace {

inline constexpr std::uint8_t kLongDcidLenOffset = 5;
inline constexpr std::uint8_t kLongDcidOffset = 6;
inline constexpr std::uint8_t kShortDcidOffset = 1;
inline constexpr std::uint8_t kLongReservedBitsMask = 0x0c;
inline constexpr std::uint8_t kShortReservedBitsMask = 0x18;

[[nodiscard]] bool has_frames(const QuicPacketEncodeSpec &spec) noexcept {
    return spec.ack_frame != nullptr || (spec.frame_queue != nullptr && !spec.frame_queue->empty()) ||
           spec.frame_count != 0;
}

[[nodiscard]] common::IoResult<std::size_t> encoded_frames_len(const QuicPacketEncodeSpec &spec,
                                                               bool &ack_eliciting) noexcept {
    QuicFrame *frames = spec.frames;
    const std::size_t count = spec.frame_count;
    if (frames == nullptr && count != 0) {
        return std::unexpected(common::IoErr::Invalid);
    }

    std::size_t len = 0;
    ack_eliciting = false;
    if (spec.ack_frame != nullptr) {
        auto frame_len = quic_create_frame(nullptr, *spec.ack_frame);
        if (!frame_len) {
            return std::unexpected(frame_len.error());
        }
        len += *frame_len;
        ack_eliciting = ack_eliciting || spec.ack_frame->ack_eliciting;
    }
    if (spec.frame_queue != nullptr) {
        for (QuicFrame *frame = spec.frame_queue->front(); frame != nullptr;
             frame = spec.frame_queue->next_of(*frame)) {
            auto frame_len = quic_create_frame(nullptr, *frame);
            if (!frame_len) {
                return std::unexpected(frame_len.error());
            }
            len += *frame_len;
            ack_eliciting = ack_eliciting || frame->ack_eliciting;
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        auto frame_len = quic_create_frame(nullptr, frames[i]);
        if (!frame_len) {
            return std::unexpected(frame_len.error());
        }
        len += *frame_len;
        ack_eliciting = ack_eliciting || frames[i].ack_eliciting;
    }
    return len;
}

[[nodiscard]] common::IoResult<void> encode_frames(QuicWriteCursor &out, const QuicPacketEncodeSpec &spec) noexcept {
    QuicFrame *frames = spec.frames;
    const std::size_t count = spec.frame_count;
    if (frames == nullptr && count != 0) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (spec.ack_frame != nullptr) {
        auto written = quic_create_frame(&out, *spec.ack_frame);
        if (!written) {
            return std::unexpected(written.error());
        }
    }
    if (spec.frame_queue != nullptr) {
        for (QuicFrame *frame = spec.frame_queue->front(); frame != nullptr;
             frame = spec.frame_queue->next_of(*frame)) {
            auto written = quic_create_frame(&out, *frame);
            if (!written) {
                return std::unexpected(written.error());
            }
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        auto written = quic_create_frame(&out, frames[i]);
        if (!written) {
            return std::unexpected(written.error());
        }
    }
    return {};
}

[[nodiscard]] QuicPacketProtectionKeys *keys_for_packet(QuicConnection &connection, QuicEncryptionLevel level,
                                                        bool write_keys) noexcept {
    return quic_packet_keys(connection.crypto(), level, write_keys);
}

} // namespace

common::IoResult<std::size_t> quic_create_version_negotiation_packet(const QuicPacketHeader &request,
                                                                     QuicWriteCursor &out) noexcept {
    if (!request.long_header) {
        return std::unexpected(common::IoErr::Invalid);
    }

    const std::size_t start = out.offset();
    auto wrote = out.write_u8(request.flags);
    if (!wrote) {
        return std::unexpected(wrote.error());
    }
    wrote = out.write_be32(0);
    if (!wrote) {
        return std::unexpected(wrote.error());
    }
    wrote = out.write_u8(request.dcid.length);
    if (!wrote) {
        return std::unexpected(wrote.error());
    }
    wrote = out.write_bytes(request.dcid.data(), request.dcid.size());
    if (!wrote) {
        return std::unexpected(wrote.error());
    }
    wrote = out.write_u8(request.scid.length);
    if (!wrote) {
        return std::unexpected(wrote.error());
    }
    wrote = out.write_bytes(request.scid.data(), request.scid.size());
    if (!wrote) {
        return std::unexpected(wrote.error());
    }
    wrote = out.write_be32(kQuicVersion1);
    if (!wrote) {
        return std::unexpected(wrote.error());
    }
    return out.offset() - start;
}

common::IoResult<std::size_t> quic_create_retry_packet(const QuicRetryPacketSpec &spec, QuicWriteCursor &out) noexcept {
    if (spec.token.empty() || (spec.token.data == nullptr && spec.token.len != 0)) {
        return std::unexpected(common::IoErr::Invalid);
    }

    const std::size_t start = out.offset();
    auto wrote = out.write_u8(kPacketFlagLong | kPacketFlagFixed | kLongPacketTypeRetry);
    if (!wrote) {
        return std::unexpected(wrote.error());
    }
    wrote = out.write_be32(spec.version);
    if (!wrote) {
        return std::unexpected(wrote.error());
    }
    wrote = out.write_u8(spec.dcid.length);
    if (!wrote) {
        return std::unexpected(wrote.error());
    }
    wrote = out.write_bytes(spec.dcid.data(), spec.dcid.size());
    if (!wrote) {
        return std::unexpected(wrote.error());
    }
    wrote = out.write_u8(spec.scid.length);
    if (!wrote) {
        return std::unexpected(wrote.error());
    }
    wrote = out.write_bytes(spec.scid.data(), spec.scid.size());
    if (!wrote) {
        return std::unexpected(wrote.error());
    }
    wrote = out.write_bytes(spec.token.data, spec.token.len);
    if (!wrote) {
        return std::unexpected(wrote.error());
    }

    std::uint8_t tag[kAeadTagLength]{};
    const std::size_t retry_without_tag_len = out.offset() - start;
    auto tagged = quic_create_retry_integrity_tag(spec.original_dcid, out.begin() + start, retry_without_tag_len, tag,
                                                  sizeof(tag));
    if (!tagged) {
        return std::unexpected(tagged.error());
    }
    wrote = out.write_bytes(tag, sizeof(tag));
    if (!wrote) {
        return std::unexpected(wrote.error());
    }
    return out.offset() - start;
}

common::IoResult<QuicConnectionId> quic_get_packet_dcid(const std::uint8_t *datagram, std::size_t datagram_len,
                                                        std::uint8_t short_dcid_len) noexcept {
    if (datagram == nullptr || datagram_len == 0) {
        return std::unexpected(common::IoErr::Invalid);
    }

    std::size_t len = short_dcid_len;
    std::size_t offset = kShortDcidOffset;
    if (quic_is_long_packet(datagram[0])) {
        if (datagram_len < kLongDcidLenOffset + 1) {
            return std::unexpected(common::IoErr::Invalid);
        }
        len = datagram[kLongDcidLenOffset];
        offset = kLongDcidOffset;
    }
    if (len > kMaxConnectionIdLength || datagram_len < offset + len) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return QuicConnectionId::from_bytes(datagram + offset, len);
}

common::IoResult<QuicPacketEncodeResult> quic_encode_packet(QuicConnection &connection,
                                                            const QuicPacketEncodeSpec &spec, std::uint8_t *out,
                                                            std::size_t out_cap) noexcept {
    if (out == nullptr || !has_frames(spec)) {
        return std::unexpected(common::IoErr::Invalid);
    }

    auto *keys = keys_for_packet(connection, spec.level, true);
    if (keys == nullptr || !keys->ready) {
        return std::unexpected(common::IoErr::NotFound);
    }

    bool ack_eliciting = false;
    auto payload_len = encoded_frames_len(spec, ack_eliciting);
    if (!payload_len) {
        return std::unexpected(payload_len.error());
    }

    const std::size_t target_len = std::max(spec.min_packet_len, *payload_len + kAeadTagLength + 64U);
    auto &space = connection.packet_number_space(spec.level);
    const auto pn_snapshot = quic_preserve_packet_number(space);

    QuicPacketHeader packet{};
    quic_init_packet_header(packet, space);
    packet.version = kQuicVersion1;
    packet.dcid = spec.dcid;
    packet.scid = spec.scid;
    packet.token = spec.token;
    packet.packet_number = quic_use_next_packet_number(space);
    packet.truncated_pn = quic_truncate_packet_number(packet.packet_number, packet.pn_len);
    packet.length = packet.pn_len + *payload_len + kAeadTagLength;

    const std::size_t cap = quic_packet_payload_capacity(packet, target_len);
    if (cap > *payload_len) {
        *payload_len = cap;
        packet.length = packet.pn_len + *payload_len + kAeadTagLength;
    }

    QuicWriteCursor writer(out, out_cap);
    std::uint8_t *pn = nullptr;
    auto header_len = quic_create_packet_header(writer, packet, &pn);
    if (!header_len) {
        quic_restore_packet_number(space, pn_snapshot);
        return std::unexpected(header_len.error());
    }

    std::array<std::uint8_t, 4096> plaintext{};
    if (*payload_len > plaintext.size()) {
        quic_restore_packet_number(space, pn_snapshot);
        return std::unexpected(common::IoErr::MessageTooLarge);
    }

    QuicWriteCursor payload_writer(plaintext.data(), *payload_len);
    auto encoded = encode_frames(payload_writer, spec);
    if (!encoded) {
        quic_restore_packet_number(space, pn_snapshot);
        return std::unexpected(encoded.error());
    }
    if (payload_writer.offset() < *payload_len) {
        auto padded = payload_writer.fill(0, *payload_len - payload_writer.offset());
        if (!padded) {
            quic_restore_packet_number(space, pn_snapshot);
            return std::unexpected(padded.error());
        }
    }

    packet.packet_data = out;
    packet.packet_len = *header_len + *payload_len + kAeadTagLength;
    packet.protected_pn = pn;
    packet.ciphertext = pn + packet.pn_len;
    packet.ciphertext_len = *payload_len + kAeadTagLength;
    if (packet.packet_len > out_cap) {
        quic_restore_packet_number(space, pn_snapshot);
        return std::unexpected(common::IoErr::NoMem);
    }

    auto sealed = quic_encrypt_packet_payload(packet, *keys, plaintext.data(), *payload_len, out + *header_len,
                                              out_cap - *header_len);
    if (!sealed) {
        quic_restore_packet_number(space, pn_snapshot);
        return std::unexpected(sealed.error());
    }
    packet.packet_len = *header_len + *sealed;

    auto protected_header = quic_apply_header_protection(packet, *keys, out, packet.packet_len);
    if (!protected_header) {
        quic_restore_packet_number(space, pn_snapshot);
        return std::unexpected(protected_header.error());
    }

    QuicPacketEncodeResult result{};
    result.packet_len = packet.packet_len;
    result.packet_number = packet.packet_number;
    result.frame_count = static_cast<std::uint32_t>(spec.frame_count);
    result.ack_eliciting = ack_eliciting;
    return result;
}

common::IoResult<QuicPacketDecodeResult> quic_decode_packet(QuicConnection &connection, std::uint8_t *datagram,
                                                            std::size_t datagram_len, std::uint8_t short_dcid_len,
                                                            std::uint8_t *plaintext,
                                                            std::size_t plaintext_cap) noexcept {
    if (datagram == nullptr || plaintext == nullptr) {
        return std::unexpected(common::IoErr::Invalid);
    }

    auto packet = quic_parse_packet_header(datagram, datagram_len, short_dcid_len);
    if (!packet) {
        return std::unexpected(packet.error());
    }

    auto *keys = keys_for_packet(connection, packet->level, false);
    if (keys == nullptr || !keys->ready) {
        return std::unexpected(common::IoErr::NotFound);
    }

    auto &space = connection.packet_number_space(packet->level);
    const std::uint64_t saved_largest_received = space.largest_received_packet_number;
    const std::uint64_t saved_pending_ack = space.pending_ack;
    const std::uint32_t saved_send_ack_count = space.send_ack_count;
    const bool saved_send_ack = space.send_ack;
    auto opened =
            quic_decrypt_packet_payload(*packet, space, *keys, datagram, packet->packet_len, plaintext, plaintext_cap);
    if (!opened) {
        return std::unexpected(opened.error());
    }
    if (opened->empty()) {
        space.largest_received_packet_number = saved_largest_received;
        space.pending_ack = saved_pending_ack;
        space.send_ack_count = saved_send_ack_count;
        space.send_ack = saved_send_ack;
        return std::unexpected(common::IoErr::Invalid);
    }
    const std::uint8_t reserved_mask = packet->long_header ? kLongReservedBitsMask : kShortReservedBitsMask;
    if ((packet->flags & reserved_mask) != 0) {
        space.largest_received_packet_number = saved_largest_received;
        space.pending_ack = saved_pending_ack;
        space.send_ack_count = saved_send_ack_count;
        space.send_ack = saved_send_ack;
        return std::unexpected(common::IoErr::Invalid);
    }

    QuicPacketDecodeResult result{};
    result.header = *packet;
    result.payload = *opened;

    QuicReadCursor payload_reader(opened->data, opened->len);
    while (!payload_reader.empty()) {
        auto parsed = quic_parse_frame_for_receiver(connection.role(), packet->level, payload_reader);
        if (!parsed) {
            space.largest_received_packet_number = saved_largest_received;
            space.pending_ack = saved_pending_ack;
            space.send_ack_count = saved_send_ack_count;
            space.send_ack = saved_send_ack;
            return std::unexpected(parsed.error());
        }
        ++result.frame_count;
        result.ack_eliciting = result.ack_eliciting || parsed->frame.ack_eliciting;
    }

    return result;
}

} // namespace fiber::quic
