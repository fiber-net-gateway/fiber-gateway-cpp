#include "QuicPacketCodec.h"

#include <algorithm>
#include <cstring>
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
    return spec.payload_len != 0 || (spec.frame_queue != nullptr && !spec.frame_queue->empty()) ||
           spec.frame_count != 0;
}

[[nodiscard]] common::IoResult<std::size_t> encoded_frames_len(const QuicPacketEncodeSpec &spec, bool &ack_eliciting,
                                                               std::size_t &frame_count) noexcept {
    QuicOutputFrame *frames = spec.frames;
    const std::size_t count = spec.frame_count;
    if (frames == nullptr && count != 0) {
        return std::unexpected(common::IoErr::Invalid);
    }

    std::size_t len = 0;
    ack_eliciting = false;
    frame_count = 0;
    if (spec.frame_queue != nullptr) {
        for (QuicOutputFrame *frame = spec.frame_queue->front(); frame != nullptr;
             frame = spec.frame_queue->next_of(*frame)) {
            auto frame_len = quic_output_frame_encoded_len(*frame);
            if (!frame_len) {
                return std::unexpected(frame_len.error());
            }
            len += *frame_len;
            ack_eliciting = ack_eliciting || quic_output_frame_ack_eliciting(frame->type);
            ++frame_count;
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        auto frame_len = quic_output_frame_encoded_len(frames[i]);
        if (!frame_len) {
            return std::unexpected(frame_len.error());
        }
        len += *frame_len;
        ack_eliciting = ack_eliciting || quic_output_frame_ack_eliciting(frames[i].type);
        ++frame_count;
    }
    return len;
}

[[nodiscard]] common::IoResult<void> encode_frames(QuicWriteCursor &out, const QuicPacketEncodeSpec &spec) noexcept {
    QuicOutputFrame *frames = spec.frames;
    const std::size_t count = spec.frame_count;
    if (frames == nullptr && count != 0) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (spec.frame_queue != nullptr) {
        for (QuicOutputFrame *frame = spec.frame_queue->front(); frame != nullptr;
             frame = spec.frame_queue->next_of(*frame)) {
            auto written = quic_create_output_frame(&out, *frame);
            if (!written) {
                return std::unexpected(written.error());
            }
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        auto written = quic_create_output_frame(&out, frames[i]);
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
                                                            const QuicPacketEncodeSpec &spec,
                                                            QuicPacketPlaintext plaintext, std::uint8_t *out,
                                                            std::size_t out_cap) noexcept {
    if (out == nullptr || plaintext.data == nullptr || plaintext.capacity == 0 || !has_frames(spec)) {
        return std::unexpected(common::IoErr::Invalid);
    }

    auto *keys = keys_for_packet(connection, spec.level, true);
    if (keys == nullptr || !keys->ready) {
        return std::unexpected(common::IoErr::NotFound);
    }

    const bool raw_payload = spec.payload != nullptr || spec.payload_len != 0;
    if (raw_payload && (spec.payload == nullptr || spec.frame_queue != nullptr || spec.frames != nullptr ||
                        spec.frame_count != 0 || spec.payload_frame_count == 0)) {
        return std::unexpected(common::IoErr::Invalid);
    }

    bool ack_eliciting = spec.payload_ack_eliciting;
    std::size_t frame_count = spec.payload_frame_count;
    std::size_t payload_len = spec.payload_len;
    if (!raw_payload) {
        auto measured_payload_len = encoded_frames_len(spec, ack_eliciting, frame_count);
        if (!measured_payload_len) {
            return std::unexpected(measured_payload_len.error());
        }
        payload_len = *measured_payload_len;
    }

    const std::size_t target_len = spec.max_packet_len == 0 ? out_cap : std::min(spec.max_packet_len, out_cap);
    auto &space = connection.packet_number_space(spec.level);
    const auto pn_snapshot = quic_preserve_packet_number(space);

    QuicPacketHeader packet{};
    quic_init_packet_header(packet, space, connection.key_phase());
    packet.version = kQuicVersion1;
    packet.dcid = spec.dcid;
    packet.scid = spec.scid;
    packet.token = spec.token;
    packet.packet_number = quic_use_next_packet_number(space);
    packet.truncated_pn = quic_truncate_packet_number(packet.packet_number, packet.pn_len);
    packet.length = packet.pn_len + payload_len + kAeadTagLength;

    const std::size_t min_payload = std::max(quic_packet_payload_capacity(packet, spec.min_packet_len),
                                             static_cast<std::size_t>(4U - packet.pn_len));
    const std::size_t max_payload = quic_packet_payload_capacity(packet, target_len);
    if (min_payload > max_payload || payload_len > max_payload) {
        quic_restore_packet_number(space, pn_snapshot);
        return std::unexpected(common::IoErr::NoMem);
    }
    const std::size_t padded_payload_len = std::max(payload_len, min_payload);
    packet.length = packet.pn_len + padded_payload_len + kAeadTagLength;

    if (padded_payload_len > plaintext.capacity) {
        quic_restore_packet_number(space, pn_snapshot);
        return std::unexpected(common::IoErr::NoMem);
    }

    QuicWriteCursor writer(out, out_cap);
    std::uint8_t *pn = nullptr;
    auto header_len = quic_create_packet_header(writer, packet, &pn);
    if (!header_len) {
        quic_restore_packet_number(space, pn_snapshot);
        return std::unexpected(header_len.error());
    }

    QuicWriteCursor payload_writer(plaintext.data, padded_payload_len);
    if (raw_payload) {
        if (spec.payload != plaintext.data) {
            std::memmove(plaintext.data, spec.payload, payload_len);
        }
    } else {
        auto encoded = encode_frames(payload_writer, spec);
        if (!encoded) {
            quic_restore_packet_number(space, pn_snapshot);
            return std::unexpected(encoded.error());
        }
        payload_len = payload_writer.offset();
    }
    if (payload_len < padded_payload_len) {
        QuicWriteCursor padding_writer(plaintext.data + payload_len, padded_payload_len - payload_len);
        auto padded = padding_writer.fill(0, padded_payload_len - payload_len);
        if (!padded) {
            quic_restore_packet_number(space, pn_snapshot);
            return std::unexpected(padded.error());
        }
    }

    packet.packet_data = out;
    packet.packet_len = *header_len + padded_payload_len + kAeadTagLength;
    packet.protected_pn = pn;
    packet.ciphertext = pn + packet.pn_len;
    packet.ciphertext_len = padded_payload_len + kAeadTagLength;
    if (packet.packet_len > out_cap) {
        quic_restore_packet_number(space, pn_snapshot);
        return std::unexpected(common::IoErr::NoMem);
    }

    auto sealed = quic_encrypt_packet_payload(packet, *keys, plaintext.data, padded_payload_len, out + *header_len,
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
    result.frame_count = static_cast<std::uint32_t>(frame_count);
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

    auto restore_space = [&]() noexcept {
        space.largest_received_packet_number = saved_largest_received;
        space.pending_ack = saved_pending_ack;
        space.send_ack_count = saved_send_ack_count;
        space.send_ack = saved_send_ack;
    };

    bool key_update = false;
    QuicSlice opened_payload{};

    if (!packet->long_header && packet->level == QuicEncryptionLevel::Application) {
        // RFC 9001 §6.3 — header protection must be removed before the key_phase
        // bit becomes visible. Remove HP first (HP keys are not rotated by key
        // update), then inspect the bit and choose the AEAD key set accordingly.
        auto unprotected = quic_remove_header_protection(*packet, *keys, datagram, packet->packet_len);
        if (!unprotected) {
            return std::unexpected(unprotected.error());
        }
        auto read_pn = quic_read_packet_number(*packet, space);
        if (!read_pn) {
            return std::unexpected(read_pn.error());
        }

        const bool wire_key_phase = (packet->flags & kPacketFlagKeyPhase) != 0;
        const QuicPacketProtectionKeys *aead_keys = keys;
        if (wire_key_phase != connection.key_phase()) {
            if (!connection.next_keys_ready()) {
                // Peer initiated a key update before we derived the next keys —
                // RFC 9001 §6: this is a fatal KEY_UPDATE_ERROR.
                connection.close(QuicErrorCode::KeyUpdateError);
                return std::unexpected(common::IoErr::Invalid);
            }
            aead_keys = &connection.crypto().next_application_read;
            key_update = true;
        }

        auto opened = quic_decrypt_aead_payload(*packet, *aead_keys, plaintext, plaintext_cap);
        if (opened) {
            opened_payload = *opened;
            space.record_received_packet_number(packet->packet_number);
        } else if (!key_update && connection.crypto().previous_application_keys_ready) {
            // Trial decryption — packet may belong to the previous key generation
            // that is still within the grace window (RFC 9001 §6.5).
            auto retry = quic_decrypt_aead_payload(*packet, connection.crypto().previous_application_read, plaintext,
                                                   plaintext_cap);
            if (!retry) {
                restore_space();
                return std::unexpected(retry.error());
            }
            opened_payload = *retry;
            space.record_received_packet_number(packet->packet_number);
        } else {
            restore_space();
            return std::unexpected(opened.error());
        }
    } else {
        auto opened = quic_decrypt_packet_payload(*packet, space, *keys, datagram, packet->packet_len, plaintext,
                                                  plaintext_cap);
        if (!opened) {
            return std::unexpected(opened.error());
        }
        opened_payload = *opened;
    }

    if (opened_payload.empty()) {
        restore_space();
        return std::unexpected(common::IoErr::Invalid);
    }
    const std::uint8_t reserved_mask = packet->long_header ? kLongReservedBitsMask : kShortReservedBitsMask;
    if ((packet->flags & reserved_mask) != 0) {
        restore_space();
        return std::unexpected(common::IoErr::Invalid);
    }

    QuicPacketDecodeResult result{};
    result.header = *packet;
    result.payload = opened_payload;
    result.key_update = key_update;

    QuicReadCursor payload_reader(opened_payload.data, opened_payload.len);
    while (!payload_reader.empty()) {
        auto parsed = quic_parse_frame_for_receiver(connection.role(), packet->level, payload_reader);
        if (!parsed) {
            restore_space();
            return std::unexpected(parsed.error());
        }
        ++result.frame_count;
        result.ack_eliciting = result.ack_eliciting || parsed->frame.ack_eliciting;
    }

    return result;
}

} // namespace fiber::quic
