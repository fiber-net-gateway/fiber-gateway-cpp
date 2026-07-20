#include "QuicTransportCodec.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <expected>

namespace fiber::quic {

namespace {

constexpr std::uint8_t kStreamFrameFin = 0x01;
constexpr std::uint8_t kStreamFrameLen = 0x02;
constexpr std::uint8_t kStreamFrameOff = 0x04;

enum class FramePermission : std::uint8_t {
    Application = 0x1,
    EarlyData = 0x2,
    Handshake = 0x4,
    Initial = 0x8,
};

constexpr std::array<std::uint8_t, static_cast<std::size_t>(kLastFrameType + 1)> kFramePermissionMasks{{
        0xF, // PADDING
        0xF, // PING
        0xD, // ACK
        0xD, // ACK_ECN
        0x3, // RESET_STREAM
        0x3, // STOP_SENDING
        0xD, // CRYPTO
        0x0, // NEW_TOKEN: server-only in NGINX's server-side parser
        0x3, // STREAM
        0x3, // STREAM1
        0x3, // STREAM2
        0x3, // STREAM3
        0x3, // STREAM4
        0x3, // STREAM5
        0x3, // STREAM6
        0x3, // STREAM7
        0x3, // MAX_DATA
        0x3, // MAX_STREAM_DATA
        0x3, // MAX_STREAMS bidi
        0x3, // MAX_STREAMS uni
        0x3, // DATA_BLOCKED
        0x3, // STREAM_DATA_BLOCKED
        0x3, // STREAMS_BLOCKED bidi
        0x3, // STREAMS_BLOCKED uni
        0x3, // NEW_CONNECTION_ID
        0x3, // RETIRE_CONNECTION_ID
        0x3, // PATH_CHALLENGE
        0x1, // PATH_RESPONSE
        0xF, // CONNECTION_CLOSE
        0x3, // CONNECTION_CLOSE_APP
        0x0, // HANDSHAKE_DONE: server-only in NGINX's server-side parser
}};

[[nodiscard]] common::IoResult<void> ensure_fixed_bit(std::uint8_t flags) noexcept {
    if ((flags & kPacketFlagFixed) == 0) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return {};
}

[[nodiscard]] common::IoResult<QuicConnectionId> read_connection_id(QuicReadCursor &in, std::size_t len) noexcept {
    auto slice = in.read_slice(len);
    if (!slice) {
        return std::unexpected(slice.error());
    }
    auto cid = QuicConnectionId::from_bytes(slice->data, slice->len);
    if (!cid) {
        return std::unexpected(cid.error());
    }
    return *cid;
}

[[nodiscard]] common::IoResult<void> write_connection_id(QuicWriteCursor &out, const QuicConnectionId &cid) noexcept {
    if (cid.length > kMaxConnectionIdLength) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return out.write_bytes(cid.data(), cid.size());
}

[[nodiscard]] common::IoResult<void> write_packet_number(QuicWriteCursor &out, std::uint8_t pn_len,
                                                         std::uint32_t truncated_pn) noexcept {
    switch (pn_len) {
        case 1:
            return out.write_u8(static_cast<std::uint8_t>(truncated_pn & 0xffU));
        case 2:
            return out.write_be16(static_cast<std::uint16_t>(truncated_pn & 0xffffU));
        case 3:
            return out.write_be24(truncated_pn & 0xffffffU);
        case 4:
            return out.write_be32(truncated_pn);
        default:
            return std::unexpected(common::IoErr::Invalid);
    }
}

[[nodiscard]] common::IoResult<QuicPacketHeader> parse_short_header(const std::uint8_t *datagram,
                                                                    std::size_t datagram_len, std::uint8_t flags,
                                                                    std::uint8_t short_dcid_len) noexcept {
    if (short_dcid_len > kMaxConnectionIdLength) {
        return std::unexpected(common::IoErr::Invalid);
    }
    auto fixed = ensure_fixed_bit(flags);
    if (!fixed) {
        return std::unexpected(fixed.error());
    }

    QuicReadCursor in(datagram + 1, datagram_len - 1);
    auto dcid = read_connection_id(in, short_dcid_len);
    if (!dcid) {
        return std::unexpected(dcid.error());
    }

    QuicPacketHeader packet{};
    packet.packet_data = datagram;
    packet.packet_len = datagram_len;
    packet.protected_flags = flags;
    packet.flags = flags;
    packet.long_header = false;
    packet.type = QuicPacketType::Short;
    packet.level = QuicEncryptionLevel::Application;
    packet.dcid = *dcid;
    packet.protected_pn = in.pos();
    packet.ciphertext = in.pos();
    packet.ciphertext_len = in.remaining();
    return packet;
}

[[nodiscard]] common::IoResult<QuicPacketHeader>
parse_long_header(const std::uint8_t *datagram, std::size_t datagram_len, std::uint8_t flags) noexcept {
    auto fixed = ensure_fixed_bit(flags);
    if (!fixed) {
        return std::unexpected(fixed.error());
    }

    QuicReadCursor in(datagram + 1, datagram_len - 1);
    auto version = in.read_be32();
    if (!version) {
        return std::unexpected(version.error());
    }

    QuicPacketHeader packet{};
    packet.packet_data = datagram;
    packet.protected_flags = flags;
    packet.flags = flags;
    packet.long_header = true;
    packet.version = *version;

    auto dcid_len = in.read_u8();
    if (!dcid_len) {
        return std::unexpected(dcid_len.error());
    }
    auto dcid = read_connection_id(in, *dcid_len);
    if (!dcid) {
        return std::unexpected(dcid.error());
    }
    packet.dcid = *dcid;

    auto scid_len = in.read_u8();
    if (!scid_len) {
        return std::unexpected(scid_len.error());
    }
    auto scid = read_connection_id(in, *scid_len);
    if (!scid) {
        return std::unexpected(scid.error());
    }
    packet.scid = *scid;

    if (*version == 0) {
        if (in.remaining() == 0 || in.remaining() % sizeof(std::uint32_t) != 0) {
            return std::unexpected(common::IoErr::Invalid);
        }
        packet.type = QuicPacketType::VersionNegotiation;
        packet.version_list = {in.pos(), in.remaining()};
        packet.packet_len = datagram_len;
        return packet;
    }
    if (*version != kQuicVersion1) {
        packet.type = QuicPacketType::UnsupportedVersion;
        packet.packet_len = datagram_len;
        return packet;
    }

    const std::uint8_t long_type = flags & kPacketFlagLongTypeMask;
    if (long_type == kLongPacketTypeRetry) {
        if (in.remaining() < kAeadTagLength) {
            return std::unexpected(common::IoErr::Invalid);
        }
        packet.type = QuicPacketType::Retry;
        packet.level = QuicEncryptionLevel::Initial;
        packet.token = {in.pos(), in.remaining() - kAeadTagLength};
        packet.packet_len = datagram_len;
        return packet;
    }

    if (long_type == kLongPacketTypeInitial) {
        auto token_len = quic_parse_varint(in);
        if (!token_len) {
            return std::unexpected(token_len.error());
        }
        auto token = in.read_slice(static_cast<std::size_t>(*token_len));
        if (!token) {
            return std::unexpected(token.error());
        }
        packet.type = QuicPacketType::Initial;
        packet.level = QuicEncryptionLevel::Initial;
        packet.token = *token;

    } else if (long_type == kLongPacketTypeZeroRtt) {
        packet.type = QuicPacketType::ZeroRtt;
        packet.level = QuicEncryptionLevel::EarlyData;

    } else if (long_type == kLongPacketTypeHandshake) {
        packet.type = QuicPacketType::Handshake;
        packet.level = QuicEncryptionLevel::Handshake;

    } else {
        return std::unexpected(common::IoErr::Invalid);
    }

    auto length = quic_parse_varint(in);
    if (!length) {
        return std::unexpected(length.error());
    }
    if (*length > in.remaining()) {
        return std::unexpected(common::IoErr::Invalid);
    }

    packet.length = *length;
    packet.protected_pn = in.pos();
    packet.ciphertext = in.pos();
    packet.ciphertext_len = static_cast<std::size_t>(*length);
    packet.packet_len = in.offset() + static_cast<std::size_t>(*length) + 1;
    return packet;
}

[[nodiscard]] std::uint8_t permission_for_level(QuicEncryptionLevel level) noexcept {
    switch (level) {
        case QuicEncryptionLevel::Initial:
            return static_cast<std::uint8_t>(FramePermission::Initial);
        case QuicEncryptionLevel::Handshake:
            return static_cast<std::uint8_t>(FramePermission::Handshake);
        case QuicEncryptionLevel::EarlyData:
            return static_cast<std::uint8_t>(FramePermission::EarlyData);
        case QuicEncryptionLevel::Application:
            return static_cast<std::uint8_t>(FramePermission::Application);
    }
    return 0;
}

[[nodiscard]] common::IoResult<void> write_or_count_varint(QuicWriteCursor *out, std::uint64_t value,
                                                           std::size_t &len) noexcept {
    len += quic_varint_len(value);
    if (out == nullptr) {
        return {};
    }
    return quic_write_varint(*out, value);
}

[[nodiscard]] common::IoResult<void> write_or_count_bytes(QuicWriteCursor *out, const std::uint8_t *data,
                                                          std::size_t bytes, std::size_t &len) noexcept {
    len += bytes;
    if (out == nullptr) {
        return {};
    }
    return out->write_bytes(data, bytes);
}

[[nodiscard]] common::IoResult<void> write_or_count_padding(QuicWriteCursor *out, std::size_t bytes,
                                                            std::size_t &len) noexcept {
    len += bytes;
    if (out == nullptr) {
        return {};
    }
    return out->fill(0, bytes);
}

[[nodiscard]] common::IoResult<void> parse_close_frame(QuicReadCursor &payload, QuicInputFrame &frame) noexcept {
    auto error_code = quic_parse_varint(payload);
    if (!error_code) {
        return std::unexpected(error_code.error());
    }
    frame.u.close.error_code = *error_code;

    if (frame.type == QuicFrameType::ConnectionClose) {
        auto frame_type = quic_parse_varint(payload);
        if (!frame_type) {
            return std::unexpected(frame_type.error());
        }
        frame.u.close.frame_type = *frame_type;
    } else {
        frame.u.close.frame_type = 0;
    }

    auto reason_len = quic_parse_varint(payload);
    if (!reason_len) {
        return std::unexpected(reason_len.error());
    }
    auto reason = payload.read_slice(static_cast<std::size_t>(*reason_len));
    if (!reason) {
        return std::unexpected(reason.error());
    }
    frame.u.close.reason = *reason;
    return {};
}

} // namespace

common::IoResult<std::uint64_t> quic_parse_varint(QuicReadCursor &in) noexcept {
    auto first = in.read_u8();
    if (!first) {
        return std::unexpected(first.error());
    }

    const std::uint8_t len = static_cast<std::uint8_t>(1U << (*first >> 6U));
    std::uint64_t value = *first & 0x3fU;
    if (in.remaining() < static_cast<std::size_t>(len - 1U)) {
        return std::unexpected(common::IoErr::Invalid);
    }

    for (std::uint8_t i = 1; i < len; ++i) {
        auto byte = in.read_u8();
        if (!byte) {
            return std::unexpected(byte.error());
        }
        value = (value << 8U) | *byte;
    }
    return value;
}

common::IoResult<void> quic_write_varint(QuicWriteCursor &out, std::uint64_t value) noexcept {
    if (value > kMaxVarint) {
        return std::unexpected(common::IoErr::Invalid);
    }

    const std::size_t len = quic_varint_len(value);
    if (out.remaining() < len) {
        return std::unexpected(common::IoErr::NoMem);
    }

    switch (len) {
        case 1:
            return out.write_u8(static_cast<std::uint8_t>(value & 0x3fU));
        case 2:
            return out.write_be16(static_cast<std::uint16_t>(0x4000U | value));
        case 4:
            return out.write_be32(static_cast<std::uint32_t>(0x80000000U | value));
        case 8: {
            auto high = out.write_be32(static_cast<std::uint32_t>(0xC0000000U | (value >> 32U)));
            if (!high) {
                return high;
            }
            return out.write_be32(static_cast<std::uint32_t>(value & 0xffffffffU));
        }
        default:
            return std::unexpected(common::IoErr::Invalid);
    }
}

std::size_t quic_varint_len(std::uint64_t value) noexcept {
    if (value < (1ULL << 6U)) {
        return 1;
    }
    if (value < (1ULL << 14U)) {
        return 2;
    }
    if (value < (1ULL << 30U)) {
        return 4;
    }
    return 8;
}

common::IoResult<QuicPacketHeader> quic_parse_packet_header(const std::uint8_t *datagram, std::size_t datagram_len,
                                                            std::uint8_t short_dcid_len) noexcept {
    if (datagram == nullptr || datagram_len == 0) {
        return std::unexpected(common::IoErr::Invalid);
    }

    const std::uint8_t flags = datagram[0];
    if (quic_is_short_packet(flags)) {
        return parse_short_header(datagram, datagram_len, flags, short_dcid_len);
    }
    return parse_long_header(datagram, datagram_len, flags);
}

common::IoResult<std::size_t> quic_create_packet_header(QuicWriteCursor &out, const QuicPacketHeader &packet,
                                                        std::uint8_t **packet_number_pos) noexcept {
    if (packet_number_pos == nullptr || packet.pn_len == 0 || packet.pn_len > 4) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (packet.type == QuicPacketType::Retry || packet.type == QuicPacketType::VersionNegotiation ||
        packet.type == QuicPacketType::UnsupportedVersion) {
        return std::unexpected(common::IoErr::NotSupported);
    }

    const std::size_t start = out.offset();
    auto wrote = out.write_u8(packet.flags);
    if (!wrote) {
        return std::unexpected(wrote.error());
    }

    if (packet.long_header) {
        wrote = out.write_be32(packet.version);
        if (!wrote) {
            return std::unexpected(wrote.error());
        }
        wrote = out.write_u8(packet.dcid.length);
        if (!wrote) {
            return std::unexpected(wrote.error());
        }
        wrote = write_connection_id(out, packet.dcid);
        if (!wrote) {
            return std::unexpected(wrote.error());
        }
        wrote = out.write_u8(packet.scid.length);
        if (!wrote) {
            return std::unexpected(wrote.error());
        }
        wrote = write_connection_id(out, packet.scid);
        if (!wrote) {
            return std::unexpected(wrote.error());
        }

        if (packet.type == QuicPacketType::Initial) {
            wrote = quic_write_varint(out, packet.token.len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            wrote = out.write_bytes(packet.token.data, packet.token.len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
        }

        wrote = quic_write_varint(out, packet.length);
        if (!wrote) {
            return std::unexpected(wrote.error());
        }
    } else {
        wrote = write_connection_id(out, packet.dcid);
        if (!wrote) {
            return std::unexpected(wrote.error());
        }
    }

    *packet_number_pos = out.pos();
    wrote = write_packet_number(out, packet.pn_len, packet.truncated_pn);
    if (!wrote) {
        return std::unexpected(wrote.error());
    }
    return out.offset() - start;
}

std::uint8_t quic_packet_number_len(std::uint64_t next_packet_number,
                                    std::uint64_t largest_acked_packet_number) noexcept {
    std::uint64_t delta = 0;
    if (largest_acked_packet_number == kUnsetPacketNumber) {
        delta = next_packet_number == UINT64_MAX ? UINT64_MAX : next_packet_number + 1;
    } else if (next_packet_number >= largest_acked_packet_number) {
        delta = next_packet_number - largest_acked_packet_number;
    }

    if (delta <= 0x7fU) {
        return 1;
    }
    if (delta <= 0x7fffU) {
        return 2;
    }
    if (delta <= 0x7fffffU) {
        return 3;
    }
    return 4;
}

std::uint8_t quic_packet_number_len(const QuicPacketNumberSpace &space) noexcept {
    return quic_packet_number_len(space.next_packet_number, space.largest_acked_packet_number);
}

std::uint32_t quic_truncate_packet_number(std::uint64_t packet_number, std::uint8_t pn_len) noexcept {
    switch (pn_len) {
        case 1:
            return static_cast<std::uint32_t>(packet_number & 0xffU);
        case 2:
            return static_cast<std::uint32_t>(packet_number & 0xffffU);
        case 3:
            return static_cast<std::uint32_t>(packet_number & 0xffffffU);
        default:
            return static_cast<std::uint32_t>(packet_number & 0xffffffffU);
    }
}

common::IoResult<std::uint64_t> quic_decode_packet_number(std::uint32_t truncated_packet_number, std::uint8_t pn_len,
                                                          std::uint64_t largest_received_packet_number) noexcept {
    if (pn_len == 0 || pn_len > 4) {
        return std::unexpected(common::IoErr::Invalid);
    }

    const std::uint32_t pn_nbits = static_cast<std::uint32_t>(pn_len) * 8U;
    const std::uint64_t pn_win = 1ULL << pn_nbits;
    const std::uint64_t pn_hwin = pn_win / 2;
    const std::uint64_t pn_mask = pn_win - 1;
    const std::uint64_t expected_packet_number =
            largest_received_packet_number == kUnsetPacketNumber ? 0 : largest_received_packet_number + 1;

    std::uint64_t candidate = (expected_packet_number & ~pn_mask) | truncated_packet_number;
    if (candidate + pn_hwin <= expected_packet_number && candidate < kMaxVarint - pn_win) {
        candidate += pn_win;
    } else if (candidate > expected_packet_number + pn_hwin && candidate >= pn_win) {
        candidate -= pn_win;
    }
    return candidate;
}

common::IoResult<void> quic_read_packet_number(QuicPacketHeader &packet, const QuicPacketNumberSpace &space) noexcept {
    if (packet.protected_pn == nullptr || packet.packet_data == nullptr) {
        return std::unexpected(common::IoErr::Invalid);
    }

    const std::uint8_t pn_len = static_cast<std::uint8_t>((packet.flags & kPacketFlagPnLengthMask) + 1);
    const std::uint8_t *packet_end = packet.packet_data + packet.packet_len;
    if (packet.protected_pn > packet_end || static_cast<std::size_t>(packet_end - packet.protected_pn) < pn_len) {
        return std::unexpected(common::IoErr::Invalid);
    }

    std::uint32_t truncated = 0;
    for (std::uint8_t i = 0; i < pn_len; ++i) {
        truncated = (truncated << 8U) | packet.protected_pn[i];
    }

    auto decoded = quic_decode_packet_number(truncated, pn_len, space.largest_received_packet_number);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }

    packet.pn_len = pn_len;
    packet.truncated_pn = truncated;
    packet.packet_number = *decoded;
    packet.ciphertext = packet.protected_pn + pn_len;
    if (packet.long_header) {
        if (packet.length < pn_len) {
            return std::unexpected(common::IoErr::Invalid);
        }
        packet.ciphertext_len = static_cast<std::size_t>(packet.length - pn_len);
    } else {
        packet.ciphertext_len = static_cast<std::size_t>(packet_end - packet.ciphertext);
    }
    return {};
}

void quic_init_packet_header(QuicPacketHeader &packet, const QuicPacketNumberSpace &space, bool key_phase) noexcept {
    const std::uint8_t pn_len = quic_packet_number_len(space);
    const std::uint8_t pn_bits = static_cast<std::uint8_t>(pn_len - 1);

    packet.level = space.level;
    packet.packet_number = space.next_packet_number;
    packet.pn_len = pn_len;
    packet.truncated_pn = quic_truncate_packet_number(packet.packet_number, pn_len);

    switch (space.level) {
        case QuicEncryptionLevel::Initial:
            packet.long_header = true;
            packet.type = QuicPacketType::Initial;
            packet.flags = kPacketFlagLong | kPacketFlagFixed | kLongPacketTypeInitial | pn_bits;
            break;
        case QuicEncryptionLevel::Handshake:
            packet.long_header = true;
            packet.type = QuicPacketType::Handshake;
            packet.flags = kPacketFlagLong | kPacketFlagFixed | kLongPacketTypeHandshake | pn_bits;
            break;
        case QuicEncryptionLevel::EarlyData:
            packet.long_header = true;
            packet.type = QuicPacketType::ZeroRtt;
            packet.flags = kPacketFlagLong | kPacketFlagFixed | kLongPacketTypeZeroRtt | pn_bits;
            break;
        case QuicEncryptionLevel::Application:
            packet.long_header = false;
            packet.type = QuicPacketType::Short;
            packet.flags = kPacketFlagFixed | pn_bits;
            if (key_phase) {
                packet.flags |= kPacketFlagKeyPhase;
            }
            break;
    }
    packet.protected_flags = packet.flags;
}

QuicPacketNumberSpaceSnapshot quic_preserve_packet_number(const QuicPacketNumberSpace &space) noexcept {
    return QuicPacketNumberSpaceSnapshot{space.next_packet_number};
}

void quic_restore_packet_number(QuicPacketNumberSpace &space, QuicPacketNumberSpaceSnapshot snapshot) noexcept {
    space.next_packet_number = snapshot.next_packet_number;
}

std::uint64_t quic_use_next_packet_number(QuicPacketNumberSpace &space) noexcept {
    const std::uint64_t packet_number = space.next_packet_number;
    ++space.next_packet_number;
    return packet_number;
}

std::size_t quic_packet_payload_capacity(const QuicPacketHeader &packet, std::size_t target_packet_len) noexcept {
    std::size_t len = 0;

    if (!packet.long_header) {
        len = 1 + packet.dcid.size() + packet.pn_len + kAeadTagLength;
        return len > target_packet_len ? 0 : target_packet_len - len;
    }

    len = 1 + 4 + 1 + packet.dcid.size() + 1 + packet.scid.size();
    if (packet.type == QuicPacketType::Initial) {
        len += quic_varint_len(packet.token.len) + packet.token.len;
    }
    if (len > target_packet_len) {
        return 0;
    }

    len += quic_varint_len(target_packet_len - len) + packet.pn_len + kAeadTagLength;
    if (len > target_packet_len) {
        return 0;
    }
    return target_packet_len - len;
}

bool quic_frame_allowed(QuicEncryptionLevel level, QuicFrameType frame_type) noexcept {
    const std::uint64_t type = static_cast<std::uint64_t>(frame_type);
    if (type > kLastFrameType) {
        return false;
    }
    return (permission_for_level(level) & kFramePermissionMasks[static_cast<std::size_t>(type)]) != 0;
}

bool quic_frame_allowed_for_receiver(QuicConnectionRole receiver_role, QuicEncryptionLevel level,
                                     QuicFrameType frame_type) noexcept {
    if (quic_frame_allowed(level, frame_type)) {
        return true;
    }
    if (receiver_role != QuicConnectionRole::Client || level != QuicEncryptionLevel::Application) {
        return false;
    }
    return frame_type == QuicFrameType::NewToken || frame_type == QuicFrameType::HandshakeDone;
}

common::IoResult<QuicInputFrameParseResult> quic_parse_frame(QuicEncryptionLevel level,
                                                             QuicReadCursor &payload) noexcept {
    return quic_parse_frame_for_receiver(QuicConnectionRole::Server, level, payload);
}

common::IoResult<QuicInputFrameParseResult> quic_parse_frame_for_receiver(QuicConnectionRole receiver_role,
                                                                          QuicEncryptionLevel level,
                                                                          QuicReadCursor &payload) noexcept {
    const std::size_t start = payload.offset();
    auto type_value = quic_parse_varint(payload);
    if (!type_value || payload.offset() - start != quic_varint_len(*type_value) || *type_value > kLastFrameType) {
        return std::unexpected(common::IoErr::Invalid);
    }

    QuicInputFrame frame{};
    frame.type = static_cast<QuicFrameType>(*type_value);
    frame.level = level;
    frame.ack_eliciting = frame.type != QuicFrameType::Padding && frame.type != QuicFrameType::Ack &&
                          frame.type != QuicFrameType::AckEcn && frame.type != QuicFrameType::ConnectionClose &&
                          frame.type != QuicFrameType::ConnectionCloseApp;

    if (!quic_frame_allowed_for_receiver(receiver_role, level, frame.type)) {
        return std::unexpected(common::IoErr::Invalid);
    }

    switch (frame.type) {
        case QuicFrameType::Padding: {
            const std::uint8_t *padding_end = payload.pos();
            while (static_cast<std::size_t>(payload.end() - padding_end) >= sizeof(std::uint64_t)) {
                std::uint64_t padding_word = 0;
                std::memcpy(&padding_word, padding_end, sizeof(padding_word));
                if (padding_word != 0) {
                    break;
                }
                padding_end += sizeof(padding_word);
            }
            while (padding_end != payload.end() && *padding_end == 0) {
                ++padding_end;
            }
            auto skipped = payload.skip(static_cast<std::size_t>(padding_end - payload.pos()));
            if (!skipped) {
                return std::unexpected(skipped.error());
            }
            frame.u.padding.length = payload.offset() - start;
            break;
        }

        case QuicFrameType::Ping:
            break;

        case QuicFrameType::Ack:
        case QuicFrameType::AckEcn: {
            auto largest = quic_parse_varint(payload);
            auto delay = largest ? quic_parse_varint(payload) : std::unexpected(largest.error());
            auto range_count = delay ? quic_parse_varint(payload) : std::unexpected(delay.error());
            auto first_range = range_count ? quic_parse_varint(payload) : std::unexpected(range_count.error());
            if (!first_range) {
                return std::unexpected(first_range.error());
            }
            frame.u.ack.largest = *largest;
            frame.u.ack.delay = *delay;
            frame.u.ack.range_count = *range_count;
            frame.u.ack.first_range = *first_range;

            const std::uint8_t *ranges = payload.pos();
            for (std::uint64_t i = 0; i < frame.u.ack.range_count; ++i) {
                auto gap = quic_parse_varint(payload);
                auto range = gap ? quic_parse_varint(payload) : std::unexpected(gap.error());
                if (!range) {
                    return std::unexpected(range.error());
                }
            }
            frame.u.ack.ranges_length = static_cast<std::uint64_t>(payload.pos() - ranges);
            frame.data = {ranges, static_cast<std::size_t>(frame.u.ack.ranges_length)};

            if (frame.type == QuicFrameType::AckEcn) {
                auto ect0 = quic_parse_varint(payload);
                auto ect1 = ect0 ? quic_parse_varint(payload) : std::unexpected(ect0.error());
                auto ce = ect1 ? quic_parse_varint(payload) : std::unexpected(ect1.error());
                if (!ce) {
                    return std::unexpected(ce.error());
                }
                frame.u.ack.ect0 = *ect0;
                frame.u.ack.ect1 = *ect1;
                frame.u.ack.ce = *ce;
            }
            break;
        }

        case QuicFrameType::Crypto: {
            auto offset = quic_parse_varint(payload);
            auto length = offset ? quic_parse_varint(payload) : std::unexpected(offset.error());
            if (!length) {
                return std::unexpected(length.error());
            }
            auto data = payload.read_slice(static_cast<std::size_t>(*length));
            if (!data) {
                return std::unexpected(data.error());
            }
            frame.u.crypto.offset = *offset;
            frame.u.crypto.length = *length;
            frame.data = *data;
            break;
        }

        case QuicFrameType::NewToken: {
            auto length = quic_parse_varint(payload);
            if (!length) {
                return std::unexpected(length.error());
            }
            auto data = payload.read_slice(static_cast<std::size_t>(*length));
            if (!data) {
                return std::unexpected(data.error());
            }
            frame.u.new_token.length = *length;
            frame.data = *data;
            break;
        }

        case QuicFrameType::Stream:
        case QuicFrameType::Stream1:
        case QuicFrameType::Stream2:
        case QuicFrameType::Stream3:
        case QuicFrameType::Stream4:
        case QuicFrameType::Stream5:
        case QuicFrameType::Stream6:
        case QuicFrameType::Stream7: {
            const std::uint8_t stream_bits = static_cast<std::uint8_t>(*type_value & 0x07U);
            frame.u.stream.fin = (stream_bits & kStreamFrameFin) != 0;
            frame.u.stream.has_offset = (stream_bits & kStreamFrameOff) != 0;
            frame.u.stream.has_length = (stream_bits & kStreamFrameLen) != 0;

            auto stream_id = quic_parse_varint(payload);
            if (!stream_id) {
                return std::unexpected(stream_id.error());
            }
            frame.u.stream.stream_id = *stream_id;

            if (frame.u.stream.has_offset) {
                auto offset = quic_parse_varint(payload);
                if (!offset) {
                    return std::unexpected(offset.error());
                }
                frame.u.stream.offset = *offset;
            } else {
                frame.u.stream.offset = 0;
            }

            if (frame.u.stream.has_length) {
                auto length = quic_parse_varint(payload);
                if (!length) {
                    return std::unexpected(length.error());
                }
                frame.u.stream.length = *length;
            } else {
                frame.u.stream.length = payload.remaining();
            }

            auto data = payload.read_slice(static_cast<std::size_t>(frame.u.stream.length));
            if (!data) {
                return std::unexpected(data.error());
            }
            frame.data = *data;
            frame.type = QuicFrameType::Stream;
            break;
        }

        case QuicFrameType::ResetStream: {
            auto id = quic_parse_varint(payload);
            auto code = id ? quic_parse_varint(payload) : std::unexpected(id.error());
            auto final_size = code ? quic_parse_varint(payload) : std::unexpected(code.error());
            if (!final_size) {
                return std::unexpected(final_size.error());
            }
            frame.u.reset_stream.id = *id;
            frame.u.reset_stream.error_code = *code;
            frame.u.reset_stream.final_size = *final_size;
            break;
        }

        case QuicFrameType::StopSending: {
            auto id = quic_parse_varint(payload);
            auto code = id ? quic_parse_varint(payload) : std::unexpected(id.error());
            if (!code) {
                return std::unexpected(code.error());
            }
            frame.u.stop_sending.id = *id;
            frame.u.stop_sending.error_code = *code;
            break;
        }

        case QuicFrameType::MaxData: {
            auto max_data = quic_parse_varint(payload);
            if (!max_data) {
                return std::unexpected(max_data.error());
            }
            frame.u.max_data.max_data = *max_data;
            break;
        }

        case QuicFrameType::MaxStreamData: {
            auto id = quic_parse_varint(payload);
            auto limit = id ? quic_parse_varint(payload) : std::unexpected(id.error());
            if (!limit) {
                return std::unexpected(limit.error());
            }
            frame.u.max_stream_data.id = *id;
            frame.u.max_stream_data.limit = *limit;
            break;
        }

        case QuicFrameType::MaxStreamsBidi:
        case QuicFrameType::MaxStreamsUni: {
            auto limit = quic_parse_varint(payload);
            if (!limit || *limit > kQuicMaxStreamLimit) {
                return std::unexpected(common::IoErr::Invalid);
            }
            frame.u.max_streams.limit = *limit;
            frame.u.max_streams.bidirectional = frame.type == QuicFrameType::MaxStreamsBidi;
            break;
        }

        case QuicFrameType::DataBlocked: {
            auto limit = quic_parse_varint(payload);
            if (!limit) {
                return std::unexpected(limit.error());
            }
            frame.u.data_blocked.limit = *limit;
            break;
        }

        case QuicFrameType::StreamDataBlocked: {
            auto id = quic_parse_varint(payload);
            auto limit = id ? quic_parse_varint(payload) : std::unexpected(id.error());
            if (!limit) {
                return std::unexpected(limit.error());
            }
            frame.u.stream_data_blocked.id = *id;
            frame.u.stream_data_blocked.limit = *limit;
            break;
        }

        case QuicFrameType::StreamsBlockedBidi:
        case QuicFrameType::StreamsBlockedUni: {
            auto limit = quic_parse_varint(payload);
            if (!limit || *limit > kQuicMaxStreamLimit) {
                return std::unexpected(common::IoErr::Invalid);
            }
            frame.u.streams_blocked.limit = *limit;
            frame.u.streams_blocked.bidirectional = frame.type == QuicFrameType::StreamsBlockedBidi;
            break;
        }

        case QuicFrameType::NewConnectionId: {
            auto sequence = quic_parse_varint(payload);
            auto retire = sequence ? quic_parse_varint(payload) : std::unexpected(sequence.error());
            if (!retire || *retire > *sequence) {
                return std::unexpected(common::IoErr::Invalid);
            }
            auto cid_len = payload.read_u8();
            if (!cid_len || *cid_len == 0 || *cid_len > kMaxConnectionIdLength) {
                return std::unexpected(common::IoErr::Invalid);
            }
            frame.u.new_connection_id.sequence_number = *sequence;
            frame.u.new_connection_id.retire_prior_to = *retire;
            frame.u.new_connection_id.cid_len = *cid_len;
            auto copied = payload.copy_bytes(frame.u.new_connection_id.cid, *cid_len);
            if (!copied) {
                return std::unexpected(copied.error());
            }
            copied = payload.copy_bytes(frame.u.new_connection_id.stateless_reset_token, kStatelessResetTokenLength);
            if (!copied) {
                return std::unexpected(copied.error());
            }
            break;
        }

        case QuicFrameType::RetireConnectionId: {
            auto sequence = quic_parse_varint(payload);
            if (!sequence) {
                return std::unexpected(sequence.error());
            }
            frame.u.retire_connection_id.sequence_number = *sequence;
            break;
        }

        case QuicFrameType::PathChallenge: {
            auto copied = payload.copy_bytes(frame.u.path_challenge.data, sizeof(frame.u.path_challenge.data));
            if (!copied) {
                return std::unexpected(copied.error());
            }
            break;
        }

        case QuicFrameType::PathResponse: {
            auto copied = payload.copy_bytes(frame.u.path_response.data, sizeof(frame.u.path_response.data));
            if (!copied) {
                return std::unexpected(copied.error());
            }
            break;
        }

        case QuicFrameType::ConnectionClose:
        case QuicFrameType::ConnectionCloseApp: {
            auto parsed = parse_close_frame(payload, frame);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            break;
        }

        case QuicFrameType::HandshakeDone:
            break;
    }

    QuicInputFrameParseResult result{};
    result.frame = frame;
    result.consumed = payload.offset() - start;
    return result;
}

common::IoResult<std::size_t> quic_create_output_frame(QuicWriteCursor *out, QuicOutputFrame &frame) noexcept {
    std::size_t len = 0;

    switch (frame.type) {
        case QuicFrameType::Padding: {
            const std::size_t count = std::max<std::size_t>(1, static_cast<std::size_t>(frame.u.padding.length));
            auto wrote = write_or_count_padding(out, count, len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            return len;
        }

        case QuicFrameType::Ping: {
            auto wrote = write_or_count_varint(out, static_cast<std::uint64_t>(QuicFrameType::Ping), len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            return len;
        }

        case QuicFrameType::Ack:
        case QuicFrameType::AckEcn: {
            auto wrote = write_or_count_varint(out, static_cast<std::uint64_t>(frame.type), len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            const std::uint64_t fields[] = {frame.u.ack.largest, frame.u.ack.delay, frame.u.ack.range_count,
                                            frame.u.ack.first_range};
            for (std::uint64_t field: fields) {
                wrote = write_or_count_varint(out, field, len);
                if (!wrote) {
                    return std::unexpected(wrote.error());
                }
            }
            wrote = write_or_count_bytes(out, frame.u.ack.ranges, frame.u.ack.ranges_length, len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            if (frame.type == QuicFrameType::AckEcn) {
                const std::uint64_t ecn[] = {frame.u.ack.ect0, frame.u.ack.ect1, frame.u.ack.ce};
                for (std::uint64_t field: ecn) {
                    wrote = write_or_count_varint(out, field, len);
                    if (!wrote) {
                        return std::unexpected(wrote.error());
                    }
                }
            }
            return len;
        }

        case QuicFrameType::Crypto: {
            const mem::IoBuf *crypto_data = frame.u.crypto.data;
            if (crypto_data == nullptr || !*crypto_data || crypto_data->readable() > UINT32_MAX) {
                return std::unexpected(common::IoErr::Invalid);
            }
            auto wrote = write_or_count_varint(out, static_cast<std::uint64_t>(QuicFrameType::Crypto), len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            wrote = write_or_count_varint(out, frame.u.crypto.offset, len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            wrote = write_or_count_varint(out, crypto_data->readable(), len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            wrote = write_or_count_bytes(out, crypto_data->readable_data(), crypto_data->readable(), len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            return len;
        }

        case QuicFrameType::Stream: {
            std::uint64_t type = static_cast<std::uint64_t>(QuicFrameType::Stream);
            if (frame.u.stream.offset != 0) {
                type |= kStreamFrameOff;
            }
            if (frame.u.stream.has_length) {
                type |= kStreamFrameLen;
            }
            if (frame.u.stream.fin) {
                type |= kStreamFrameFin;
            }
            auto wrote = write_or_count_varint(out, type, len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            wrote = write_or_count_varint(out, frame.u.stream.stream_id, len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            if (frame.u.stream.offset != 0) {
                wrote = write_or_count_varint(out, frame.u.stream.offset, len);
                if (!wrote) {
                    return std::unexpected(wrote.error());
                }
            }
            if (frame.u.stream.has_length) {
                wrote = write_or_count_varint(out, frame.u.stream.length, len);
                if (!wrote) {
                    return std::unexpected(wrote.error());
                }
            }
            len += frame.u.stream.length;
            if (out != nullptr && frame.u.stream.length != 0) {
                return std::unexpected(common::IoErr::NotSupported);
            }
            return len;
        }

        case QuicFrameType::ConnectionClose:
        case QuicFrameType::ConnectionCloseApp: {
            auto wrote = write_or_count_varint(out, static_cast<std::uint64_t>(frame.type), len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            wrote = write_or_count_varint(out, frame.u.close.error_code, len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            if (frame.type == QuicFrameType::ConnectionClose) {
                wrote = write_or_count_varint(out, frame.u.close.frame_type, len);
                if (!wrote) {
                    return std::unexpected(wrote.error());
                }
            }
            wrote = write_or_count_varint(out, frame.u.close.reason_length, len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            wrote = write_or_count_bytes(out, frame.u.close.reason, frame.u.close.reason_length, len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            return len;
        }

        case QuicFrameType::ResetStream: {
            auto wrote = write_or_count_varint(out, static_cast<std::uint64_t>(QuicFrameType::ResetStream), len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            const std::uint64_t fields[] = {frame.u.reset_stream.id, frame.u.reset_stream.error_code,
                                            frame.u.reset_stream.final_size};
            for (std::uint64_t field: fields) {
                wrote = write_or_count_varint(out, field, len);
                if (!wrote) {
                    return std::unexpected(wrote.error());
                }
            }
            return len;
        }

        case QuicFrameType::StopSending: {
            auto wrote = write_or_count_varint(out, static_cast<std::uint64_t>(QuicFrameType::StopSending), len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            wrote = write_or_count_varint(out, frame.u.stop_sending.id, len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            wrote = write_or_count_varint(out, frame.u.stop_sending.error_code, len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            return len;
        }

        case QuicFrameType::MaxData: {
            auto wrote = write_or_count_varint(out, static_cast<std::uint64_t>(QuicFrameType::MaxData), len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            wrote = write_or_count_varint(out, frame.u.max_data.max_data, len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            return len;
        }

        case QuicFrameType::MaxStreamData: {
            auto wrote = write_or_count_varint(out, static_cast<std::uint64_t>(QuicFrameType::MaxStreamData), len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            wrote = write_or_count_varint(out, frame.u.max_stream_data.id, len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            wrote = write_or_count_varint(out, frame.u.max_stream_data.limit, len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            return len;
        }

        case QuicFrameType::MaxStreamsBidi:
        case QuicFrameType::MaxStreamsUni: {
            const auto type = frame.type == QuicFrameType::MaxStreamsBidi ? QuicFrameType::MaxStreamsBidi
                                                                          : QuicFrameType::MaxStreamsUni;
            auto wrote = write_or_count_varint(out, static_cast<std::uint64_t>(type), len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            wrote = write_or_count_varint(out, frame.u.max_streams.limit, len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            return len;
        }

        case QuicFrameType::DataBlocked: {
            auto wrote = write_or_count_varint(out, static_cast<std::uint64_t>(QuicFrameType::DataBlocked), len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            wrote = write_or_count_varint(out, frame.u.data_blocked.limit, len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            return len;
        }

        case QuicFrameType::StreamDataBlocked: {
            auto wrote = write_or_count_varint(out, static_cast<std::uint64_t>(QuicFrameType::StreamDataBlocked), len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            wrote = write_or_count_varint(out, frame.u.stream_data_blocked.id, len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            wrote = write_or_count_varint(out, frame.u.stream_data_blocked.limit, len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            return len;
        }

        case QuicFrameType::StreamsBlockedBidi:
        case QuicFrameType::StreamsBlockedUni: {
            const auto type = frame.type == QuicFrameType::StreamsBlockedBidi ? QuicFrameType::StreamsBlockedBidi
                                                                              : QuicFrameType::StreamsBlockedUni;
            auto wrote = write_or_count_varint(out, static_cast<std::uint64_t>(type), len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            wrote = write_or_count_varint(out, frame.u.streams_blocked.limit, len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            return len;
        }

        case QuicFrameType::NewConnectionId: {
            const QuicNewConnectionIdFrame &new_connection_id = frame.u.new_connection_id;
            if (new_connection_id.cid_len == 0 || new_connection_id.cid_len > kMaxConnectionIdLength ||
                new_connection_id.retire_prior_to > new_connection_id.sequence_number) {
                return std::unexpected(common::IoErr::Invalid);
            }
            auto wrote = write_or_count_varint(out, static_cast<std::uint64_t>(QuicFrameType::NewConnectionId), len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            wrote = write_or_count_varint(out, new_connection_id.sequence_number, len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            wrote = write_or_count_varint(out, new_connection_id.retire_prior_to, len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            ++len;
            if (out != nullptr) {
                wrote = out->write_u8(new_connection_id.cid_len);
                if (!wrote) {
                    return std::unexpected(wrote.error());
                }
            }
            wrote = write_or_count_bytes(out, new_connection_id.cid, new_connection_id.cid_len, len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            wrote = write_or_count_bytes(out, new_connection_id.stateless_reset_token, kStatelessResetTokenLength, len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            return len;
        }

        case QuicFrameType::RetireConnectionId: {
            auto wrote = write_or_count_varint(out, static_cast<std::uint64_t>(QuicFrameType::RetireConnectionId), len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            wrote = write_or_count_varint(out, frame.u.retire_connection_id.sequence_number, len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            return len;
        }

        case QuicFrameType::PathChallenge:
        case QuicFrameType::PathResponse: {
            const auto type = frame.type == QuicFrameType::PathChallenge ? QuicFrameType::PathChallenge
                                                                         : QuicFrameType::PathResponse;
            const auto *bytes = frame.type == QuicFrameType::PathChallenge ? frame.u.path_challenge.data
                                                                           : frame.u.path_response.data;
            auto wrote = write_or_count_varint(out, static_cast<std::uint64_t>(type), len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            wrote = write_or_count_bytes(out, bytes, 8, len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            return len;
        }

        case QuicFrameType::HandshakeDone: {
            auto wrote = write_or_count_varint(out, static_cast<std::uint64_t>(QuicFrameType::HandshakeDone), len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            return len;
        }

        case QuicFrameType::NewToken: {
            auto wrote = write_or_count_varint(out, static_cast<std::uint64_t>(QuicFrameType::NewToken), len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            wrote = write_or_count_varint(out, frame.u.new_token.length, len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            wrote = write_or_count_bytes(out, frame.u.new_token.data, frame.u.new_token.length, len);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            return len;
        }

        case QuicFrameType::Stream1:
        case QuicFrameType::Stream2:
        case QuicFrameType::Stream3:
        case QuicFrameType::Stream4:
        case QuicFrameType::Stream5:
        case QuicFrameType::Stream6:
        case QuicFrameType::Stream7:
            return std::unexpected(common::IoErr::NotSupported);
    }

    return std::unexpected(common::IoErr::NotSupported);
}

common::IoResult<std::size_t> quic_output_frame_encoded_len(QuicOutputFrame &frame) noexcept {
    if (frame.encoded_len != 0) {
        return frame.encoded_len;
    }

    auto frame_len = quic_create_output_frame(nullptr, frame);
    if (!frame_len) {
        return std::unexpected(frame_len.error());
    }
    frame.encoded_len = *frame_len;
    return frame.encoded_len;
}

} // namespace fiber::quic
