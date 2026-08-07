#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include <fiber/quic/QuicTransportCodec.h>

namespace {

fiber::quic::QuicConnectionId cid_from(std::initializer_list<std::uint8_t> bytes) {
    auto cid = fiber::quic::QuicConnectionId::from_bytes(bytes.begin(), bytes.size());
    EXPECT_TRUE(cid.has_value());
    return cid.value_or(fiber::quic::QuicConnectionId{});
}

} // namespace

TEST(QuicTransportCodecTest, VarintRoundTripsBoundaryValues) {
    constexpr std::array<std::uint64_t, 7> values{
            63, 64, 16383, 16384, (1ULL << 30U) - 1U, (1ULL << 30U), fiber::quic::kMaxVarint,
    };

    for (std::uint64_t value: values) {
        std::array<std::uint8_t, 8> buf{};
        fiber::quic::QuicWriteCursor out(buf.data(), buf.size());

        auto written = fiber::quic::quic_write_varint(out, value);

        ASSERT_TRUE(written.has_value());
        EXPECT_EQ(out.offset(), fiber::quic::quic_varint_len(value));

        fiber::quic::QuicReadCursor in(buf.data(), out.offset());
        auto parsed = fiber::quic::quic_parse_varint(in);

        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(*parsed, value);
        EXPECT_TRUE(in.empty());
    }
}

TEST(QuicTransportCodecTest, RejectsTruncatedVarint) {
    const std::array<std::uint8_t, 1> buf{0x40};
    fiber::quic::QuicReadCursor in(buf.data(), buf.size());

    auto parsed = fiber::quic::quic_parse_varint(in);

    EXPECT_FALSE(parsed.has_value());
}

TEST(QuicTransportCodecTest, AcceptsNonMinimalVarintOutsideFrameType) {
    const std::array<std::uint8_t, 2> buf{0x40, 0x00};
    fiber::quic::QuicReadCursor in(buf.data(), buf.size());

    auto parsed = fiber::quic::quic_parse_varint(in);

    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, 0U);
    EXPECT_TRUE(in.empty());
}

TEST(QuicTransportCodecTest, RejectsNonMinimalFrameTypeEncoding) {
    {
        const std::array<std::uint8_t, 2> bytes{0x40, 0x01};
        fiber::quic::QuicReadCursor in(bytes.data(), bytes.size());
        EXPECT_FALSE(fiber::quic::quic_parse_frame(fiber::quic::QuicEncryptionLevel::Application, in).has_value());
    }
    {
        const std::array<std::uint8_t, 4> bytes{0x80, 0x00, 0x00, 0x01};
        fiber::quic::QuicReadCursor in(bytes.data(), bytes.size());
        EXPECT_FALSE(fiber::quic::quic_parse_frame(fiber::quic::QuicEncryptionLevel::Application, in).has_value());
    }
    {
        const std::array<std::uint8_t, 8> bytes{0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
        fiber::quic::QuicReadCursor in(bytes.data(), bytes.size());
        EXPECT_FALSE(fiber::quic::quic_parse_frame(fiber::quic::QuicEncryptionLevel::Application, in).has_value());
    }
}

TEST(QuicTransportCodecTest, ParsesInitialLongHeaderAndPacketLength) {
    std::vector<std::uint8_t> datagram(fiber::quic::kMinInitialDatagramSize);
    fiber::quic::QuicWriteCursor out(datagram.data(), datagram.size());

    ASSERT_TRUE(out.write_u8(fiber::quic::kPacketFlagLong | fiber::quic::kPacketFlagFixed |
                             fiber::quic::kLongPacketTypeInitial | 0x03)
                        .has_value());
    ASSERT_TRUE(out.write_be32(fiber::quic::kQuicVersion1).has_value());
    ASSERT_TRUE(out.write_u8(8).has_value());
    const std::array<std::uint8_t, 8> dcid{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    ASSERT_TRUE(out.write_bytes(dcid.data(), dcid.size()).has_value());
    ASSERT_TRUE(out.write_u8(4).has_value());
    const std::array<std::uint8_t, 4> scid{0x11, 0x12, 0x13, 0x14};
    ASSERT_TRUE(out.write_bytes(scid.data(), scid.size()).has_value());
    ASSERT_TRUE(fiber::quic::quic_write_varint(out, 0).has_value());
    ASSERT_TRUE(fiber::quic::quic_write_varint(out, 6).has_value());
    const std::size_t protected_pn_offset = out.offset();
    ASSERT_TRUE(out.write_be32(0x01020304).has_value());
    ASSERT_TRUE(out.write_u8(0xaa).has_value());
    ASSERT_TRUE(out.write_u8(0xbb).has_value());

    auto packet = fiber::quic::quic_parse_packet_header(datagram.data(), datagram.size(), 0);

    ASSERT_TRUE(packet.has_value());
    EXPECT_TRUE(packet->long_header);
    EXPECT_EQ(packet->type, fiber::quic::QuicPacketType::Initial);
    EXPECT_EQ(packet->level, fiber::quic::QuicEncryptionLevel::Initial);
    EXPECT_EQ(packet->dcid.size(), dcid.size());
    EXPECT_EQ(packet->scid.size(), scid.size());
    EXPECT_EQ(packet->token.len, 0U);
    EXPECT_EQ(packet->length, 6U);
    EXPECT_EQ(packet->packet_len, protected_pn_offset + 6U);
    EXPECT_EQ(packet->protected_pn, datagram.data() + protected_pn_offset);
}

TEST(QuicTransportCodecTest, ParsesUnsupportedVersionLongHeaderCids) {
    std::array<std::uint8_t, 32> datagram{};
    fiber::quic::QuicWriteCursor out(datagram.data(), datagram.size());

    ASSERT_TRUE(out.write_u8(fiber::quic::kPacketFlagLong | fiber::quic::kPacketFlagFixed |
                             fiber::quic::kLongPacketTypeInitial)
                        .has_value());
    ASSERT_TRUE(out.write_be32(0xfaceb00cU).has_value());
    const std::array<std::uint8_t, 4> dcid{0x01, 0x02, 0x03, 0x04};
    ASSERT_TRUE(out.write_u8(dcid.size()).has_value());
    ASSERT_TRUE(out.write_bytes(dcid.data(), dcid.size()).has_value());
    const std::array<std::uint8_t, 3> scid{0x11, 0x22, 0x33};
    ASSERT_TRUE(out.write_u8(scid.size()).has_value());
    ASSERT_TRUE(out.write_bytes(scid.data(), scid.size()).has_value());

    auto packet = fiber::quic::quic_parse_packet_header(datagram.data(), datagram.size(), 0);

    ASSERT_TRUE(packet.has_value());
    EXPECT_TRUE(packet->long_header);
    EXPECT_EQ(packet->type, fiber::quic::QuicPacketType::UnsupportedVersion);
    EXPECT_EQ(packet->version, 0xfaceb00cU);
    EXPECT_EQ(packet->dcid.size(), dcid.size());
    EXPECT_EQ(std::memcmp(packet->dcid.data(), dcid.data(), dcid.size()), 0);
    EXPECT_EQ(packet->scid.size(), scid.size());
    EXPECT_EQ(std::memcmp(packet->scid.data(), scid.data(), scid.size()), 0);
    EXPECT_EQ(packet->packet_len, datagram.size());
}

TEST(QuicTransportCodecTest, CreatesLongHeaderWithPacketNumberPointer) {
    fiber::quic::QuicPacketHeader packet{};
    packet.long_header = true;
    packet.type = fiber::quic::QuicPacketType::Initial;
    packet.level = fiber::quic::QuicEncryptionLevel::Initial;
    packet.flags =
            fiber::quic::kPacketFlagLong | fiber::quic::kPacketFlagFixed | fiber::quic::kLongPacketTypeInitial | 0x03;
    packet.version = fiber::quic::kQuicVersion1;
    packet.dcid = cid_from({0x01, 0x02});
    packet.scid = cid_from({0x03, 0x04, 0x05});
    packet.length = 8;
    packet.pn_len = 4;
    packet.truncated_pn = 0x01020304;

    std::array<std::uint8_t, 64> buf{};
    fiber::quic::QuicWriteCursor out(buf.data(), buf.size());
    std::uint8_t *pn = nullptr;

    auto len = fiber::quic::quic_create_packet_header(out, packet, &pn);

    ASSERT_TRUE(len.has_value());
    EXPECT_EQ(*len, out.offset());
    ASSERT_NE(pn, nullptr);
    EXPECT_EQ(pn[0], 0x01);
    EXPECT_EQ(pn[1], 0x02);
    EXPECT_EQ(pn[2], 0x03);
    EXPECT_EQ(pn[3], 0x04);
}

TEST(QuicTransportCodecTest, CreatesShortHeaderWithDestinationConnectionId) {
    fiber::quic::QuicPacketHeader packet{};
    packet.long_header = false;
    packet.type = fiber::quic::QuicPacketType::Short;
    packet.level = fiber::quic::QuicEncryptionLevel::Application;
    packet.flags = fiber::quic::kPacketFlagFixed;
    packet.dcid = cid_from({0x01, 0x02, 0x03, 0x04});
    packet.pn_len = 1;
    packet.truncated_pn = 0x7a;

    std::array<std::uint8_t, 16> buf{};
    fiber::quic::QuicWriteCursor out(buf.data(), buf.size());
    std::uint8_t *pn = nullptr;

    auto len = fiber::quic::quic_create_packet_header(out, packet, &pn);

    ASSERT_TRUE(len.has_value());
    EXPECT_EQ(*len, 1U + packet.dcid.size() + packet.pn_len);
    ASSERT_NE(pn, nullptr);
    EXPECT_EQ(pn, buf.data() + 1 + packet.dcid.size());
    EXPECT_EQ(buf[1], 0x01);
    EXPECT_EQ(buf[4], 0x04);
    EXPECT_EQ(*pn, 0x7a);
}


TEST(QuicTransportCodecTest, SelectsPacketNumberLengthFromLargestAcked) {
    EXPECT_EQ(fiber::quic::quic_packet_number_len(0, fiber::quic::kUnsetPacketNumber), 1U);
    EXPECT_EQ(fiber::quic::quic_packet_number_len(126, fiber::quic::kUnsetPacketNumber), 1U);
    EXPECT_EQ(fiber::quic::quic_packet_number_len(127, fiber::quic::kUnsetPacketNumber), 2U);
    EXPECT_EQ(fiber::quic::quic_packet_number_len(0x7fff, 0), 2U);
    EXPECT_EQ(fiber::quic::quic_packet_number_len(0x8000, 0), 3U);
    EXPECT_EQ(fiber::quic::quic_packet_number_len(0x800000, 0), 4U);
}

TEST(QuicTransportCodecTest, DecodesTruncatedPacketNumberFromSpaceLargestReceived) {
    auto first = fiber::quic::quic_decode_packet_number(0x7b, 1, fiber::quic::kUnsetPacketNumber);
    auto near_next_window = fiber::quic::quic_decode_packet_number(0x02, 1, 0xff);
    auto previous_window = fiber::quic::quic_decode_packet_number(0xff, 1, 0x180);

    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(near_next_window.has_value());
    ASSERT_TRUE(previous_window.has_value());
    EXPECT_EQ(*first, 0x7bU);
    EXPECT_EQ(*near_next_window, 0x102U);
    EXPECT_EQ(*previous_window, 0x1ffU);
}

TEST(QuicTransportCodecTest, ReadsPacketNumberAndMovesCiphertextPastPacketNumber) {
    const std::array<std::uint8_t, 5> datagram{0x41, 0x10, 0x02, 0xaa, 0xbb};
    fiber::quic::QuicPacketHeader packet{};
    packet.packet_data = datagram.data();
    packet.packet_len = datagram.size();
    packet.flags = datagram[0];
    packet.type = fiber::quic::QuicPacketType::Short;
    packet.level = fiber::quic::QuicEncryptionLevel::Application;
    packet.protected_pn = datagram.data() + 1;

    fiber::quic::QuicPacketNumberSpace space{};
    space.reset(fiber::quic::QuicEncryptionLevel::Application);
    space.largest_received_packet_number = 0xff;

    auto read = fiber::quic::quic_read_packet_number(packet, space);

    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(packet.pn_len, 2U);
    EXPECT_EQ(packet.truncated_pn, 0x1002U);
    EXPECT_EQ(packet.packet_number, 0x1002U);
    EXPECT_EQ(packet.ciphertext, datagram.data() + 3);
    EXPECT_EQ(packet.ciphertext_len, 2U);
}

TEST(QuicTransportCodecTest, InitializesPacketHeaderFromPacketNumberSpaceLevel) {
    fiber::quic::QuicPacketNumberSpace initial{};
    initial.reset(fiber::quic::QuicEncryptionLevel::Initial);
    initial.next_packet_number = 127;
    fiber::quic::QuicPacketHeader initial_packet{};

    fiber::quic::quic_init_packet_header(initial_packet, initial);

    EXPECT_TRUE(initial_packet.long_header);
    EXPECT_EQ(initial_packet.type, fiber::quic::QuicPacketType::Initial);
    EXPECT_EQ(initial_packet.level, fiber::quic::QuicEncryptionLevel::Initial);
    EXPECT_EQ(initial_packet.pn_len, 2U);
    EXPECT_EQ(initial_packet.truncated_pn, 127U);
    EXPECT_EQ(initial_packet.flags, fiber::quic::kPacketFlagLong | fiber::quic::kPacketFlagFixed |
                                            fiber::quic::kLongPacketTypeInitial | 0x01);

    fiber::quic::QuicPacketNumberSpace application{};
    application.reset(fiber::quic::QuicEncryptionLevel::Application);
    fiber::quic::QuicPacketHeader app_packet{};

    fiber::quic::quic_init_packet_header(app_packet, application);

    EXPECT_FALSE(app_packet.long_header);
    EXPECT_EQ(app_packet.type, fiber::quic::QuicPacketType::Short);
    EXPECT_EQ(app_packet.level, fiber::quic::QuicEncryptionLevel::Application);
    EXPECT_EQ(app_packet.flags, fiber::quic::kPacketFlagFixed);
}

TEST(QuicTransportCodecTest, PreservesAndRestoresPacketNumberForSendRollback) {
    fiber::quic::QuicPacketNumberSpace space{};
    space.reset(fiber::quic::QuicEncryptionLevel::Initial);
    auto snapshot = fiber::quic::quic_preserve_packet_number(space);

    EXPECT_EQ(fiber::quic::quic_use_next_packet_number(space), 0U);
    EXPECT_EQ(fiber::quic::quic_use_next_packet_number(space), 1U);
    ASSERT_EQ(space.next_packet_number, 2U);

    fiber::quic::quic_restore_packet_number(space, snapshot);

    EXPECT_EQ(space.next_packet_number, 0U);
}

TEST(QuicTransportCodecTest, ParsesStreamFrameWithOffsetLengthAndFin) {
    const std::array<std::uint8_t, 7> bytes{
            0x0f, // STREAM + OFF + LEN + FIN
            0x04, // stream id
            0x02, // offset
            0x03, // length
            'a',  'b', 'c',
    };
    fiber::quic::QuicReadCursor in(bytes.data(), bytes.size());

    auto parsed = fiber::quic::quic_parse_frame(fiber::quic::QuicEncryptionLevel::Application, in);

    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->consumed, bytes.size());
    EXPECT_EQ(parsed->frame.type, fiber::quic::QuicFrameType::Stream);
    EXPECT_TRUE(parsed->frame.ack_eliciting);
    EXPECT_TRUE(parsed->frame.u.stream.has_offset);
    EXPECT_TRUE(parsed->frame.u.stream.has_length);
    EXPECT_TRUE(parsed->frame.u.stream.fin);
    EXPECT_EQ(parsed->frame.u.stream.stream_id, 4U);
    EXPECT_EQ(parsed->frame.u.stream.offset, 2U);
    EXPECT_EQ(parsed->frame.u.stream.length, 3U);
    ASSERT_EQ(parsed->frame.data.len, 3U);
    EXPECT_EQ(parsed->frame.data.data[0], 'a');
}

TEST(QuicTransportCodecTest, CoalescesPaddingAcrossWordChunks) {
    std::array<std::uint8_t, 24> bytes{};
    bytes[11] = static_cast<std::uint8_t>(fiber::quic::QuicFrameType::Ping);
    fiber::quic::QuicReadCursor in(bytes.data(), bytes.size());

    auto padding = fiber::quic::quic_parse_frame(fiber::quic::QuicEncryptionLevel::Application, in);

    ASSERT_TRUE(padding.has_value());
    EXPECT_EQ(padding->frame.type, fiber::quic::QuicFrameType::Padding);
    EXPECT_EQ(padding->frame.u.padding.length, 11U);
    EXPECT_EQ(padding->consumed, 11U);

    auto ping = fiber::quic::quic_parse_frame(fiber::quic::QuicEncryptionLevel::Application, in);
    ASSERT_TRUE(ping.has_value());
    EXPECT_EQ(ping->frame.type, fiber::quic::QuicFrameType::Ping);
    EXPECT_EQ(ping->consumed, 1U);
}

TEST(QuicTransportCodecTest, CreatesAndParsesCryptoFrame) {
    const std::array<std::uint8_t, 3> payload{0xde, 0xad, 0xbe};
    fiber::quic::QuicOutputFrame frame{};
    frame.type = fiber::quic::QuicFrameType::Crypto;
    frame.u.crypto.offset = 7;
    auto owned = fiber::quic::quic_output_frame_set_owned_data(frame, payload.data(), payload.size());
    ASSERT_TRUE(owned.has_value());

    auto expected_len = fiber::quic::quic_create_output_frame(nullptr, frame);

    ASSERT_TRUE(expected_len.has_value());
    std::array<std::uint8_t, 32> buf{};
    fiber::quic::QuicWriteCursor out(buf.data(), buf.size());
    auto written = fiber::quic::quic_create_output_frame(&out, frame);

    ASSERT_TRUE(written.has_value());
    EXPECT_EQ(*written, *expected_len);
    EXPECT_EQ(out.offset(), *expected_len);

    fiber::quic::QuicReadCursor in(buf.data(), out.offset());
    auto parsed = fiber::quic::quic_parse_frame(fiber::quic::QuicEncryptionLevel::Initial, in);

    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->frame.type, fiber::quic::QuicFrameType::Crypto);
    EXPECT_EQ(parsed->frame.u.crypto.offset, 7U);
    EXPECT_EQ(parsed->frame.u.crypto.length, payload.size());
    ASSERT_EQ(parsed->frame.data.len, payload.size());
    EXPECT_EQ(parsed->frame.data.data[1], 0xad);
    fiber::quic::quic_output_frame_release_data(frame);
}

TEST(QuicTransportCodecTest, ChecksFramePermissionByEncryptionLevel) {
    EXPECT_TRUE(fiber::quic::quic_frame_allowed(fiber::quic::QuicEncryptionLevel::Initial,
                                                fiber::quic::QuicFrameType::Crypto));
    EXPECT_FALSE(fiber::quic::quic_frame_allowed(fiber::quic::QuicEncryptionLevel::Initial,
                                                 fiber::quic::QuicFrameType::Stream));
    EXPECT_FALSE(fiber::quic::quic_frame_allowed(fiber::quic::QuicEncryptionLevel::EarlyData,
                                                 fiber::quic::QuicFrameType::Ack));
    EXPECT_FALSE(fiber::quic::quic_frame_allowed(fiber::quic::QuicEncryptionLevel::EarlyData,
                                                 fiber::quic::QuicFrameType::Crypto));
    EXPECT_TRUE(fiber::quic::quic_frame_allowed(fiber::quic::QuicEncryptionLevel::EarlyData,
                                                fiber::quic::QuicFrameType::Stream));
    EXPECT_TRUE(fiber::quic::quic_frame_allowed(fiber::quic::QuicEncryptionLevel::Application,
                                                fiber::quic::QuicFrameType::PathResponse));

    const std::array<std::uint8_t, 4> stream_bytes{0x0a, 0x00, 0x01, 0x42};
    fiber::quic::QuicReadCursor stream_in(stream_bytes.data(), stream_bytes.size());
    auto stream = fiber::quic::quic_parse_frame(fiber::quic::QuicEncryptionLevel::EarlyData, stream_in);
    ASSERT_TRUE(stream.has_value());
    EXPECT_EQ(stream->frame.type, fiber::quic::QuicFrameType::Stream);
    EXPECT_EQ(stream->frame.u.stream.stream_id, 0U);
    EXPECT_EQ(stream->frame.u.stream.length, 1U);

    const std::array<std::uint8_t, 3> crypto_bytes{0x06, 0x00, 0x00};
    fiber::quic::QuicReadCursor crypto_in(crypto_bytes.data(), crypto_bytes.size());
    EXPECT_FALSE(fiber::quic::quic_parse_frame(fiber::quic::QuicEncryptionLevel::EarlyData, crypto_in).has_value());
}

TEST(QuicTransportCodecTest, CreatesAndClientParsesNewTokenFrame) {
    const std::array<std::uint8_t, 3> token{'a', 'b', 'c'};
    fiber::quic::QuicOutputFrame frame{};
    frame.type = fiber::quic::QuicFrameType::NewToken;
    frame.u.new_token.data = token.data();
    frame.u.new_token.length = token.size();

    std::array<std::uint8_t, 16> buf{};
    fiber::quic::QuicWriteCursor out(buf.data(), buf.size());
    auto written = fiber::quic::quic_create_output_frame(&out, frame);
    ASSERT_TRUE(written.has_value());

    fiber::quic::QuicReadCursor server_in(buf.data(), out.offset());
    auto server_parsed = fiber::quic::quic_parse_frame(fiber::quic::QuicEncryptionLevel::Application, server_in);
    EXPECT_FALSE(server_parsed.has_value());

    fiber::quic::QuicReadCursor client_in(buf.data(), out.offset());
    auto client_parsed = fiber::quic::quic_parse_frame_for_receiver(
            fiber::quic::QuicConnectionRole::Client, fiber::quic::QuicEncryptionLevel::Application, client_in);

    ASSERT_TRUE(client_parsed.has_value());
    EXPECT_EQ(client_parsed->frame.type, fiber::quic::QuicFrameType::NewToken);
    EXPECT_EQ(client_parsed->frame.u.new_token.length, token.size());
    EXPECT_EQ(client_parsed->frame.data.data[2], 'c');
}

TEST(QuicTransportCodecTest, ValidatesNewConnectionIdBeforeEncoding) {
    fiber::quic::QuicOutputFrame frame{};
    frame.type = fiber::quic::QuicFrameType::NewConnectionId;
    frame.u.new_connection_id.sequence_number = 3;
    frame.u.new_connection_id.retire_prior_to = 2;

    frame.u.new_connection_id.cid_len = 0;
    EXPECT_FALSE(fiber::quic::quic_create_output_frame(nullptr, frame).has_value());

    frame.u.new_connection_id.cid_len = fiber::quic::kMaxConnectionIdLength + 1;
    EXPECT_FALSE(fiber::quic::quic_create_output_frame(nullptr, frame).has_value());

    frame.u.new_connection_id.cid_len = fiber::quic::kMaxConnectionIdLength;
    frame.u.new_connection_id.retire_prior_to = frame.u.new_connection_id.sequence_number + 1;
    EXPECT_FALSE(fiber::quic::quic_create_output_frame(nullptr, frame).has_value());

    frame.u.new_connection_id.retire_prior_to = frame.u.new_connection_id.sequence_number;
    std::array<std::uint8_t, 64> buf{};
    fiber::quic::QuicWriteCursor out(buf.data(), buf.size());
    auto written = fiber::quic::quic_create_output_frame(&out, frame);
    ASSERT_TRUE(written.has_value());

    fiber::quic::QuicReadCursor in(buf.data(), out.offset());
    auto parsed = fiber::quic::quic_parse_frame(fiber::quic::QuicEncryptionLevel::Application, in);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->frame.type, fiber::quic::QuicFrameType::NewConnectionId);
    EXPECT_EQ(parsed->frame.u.new_connection_id.cid_len, fiber::quic::kMaxConnectionIdLength);
    EXPECT_EQ(parsed->frame.u.new_connection_id.retire_prior_to, parsed->frame.u.new_connection_id.sequence_number);
}

TEST(QuicTransportCodecTest, ClientParsesHandshakeDoneFrame) {
    fiber::quic::QuicOutputFrame frame{};
    frame.type = fiber::quic::QuicFrameType::HandshakeDone;

    std::array<std::uint8_t, 8> buf{};
    fiber::quic::QuicWriteCursor out(buf.data(), buf.size());
    auto written = fiber::quic::quic_create_output_frame(&out, frame);
    ASSERT_TRUE(written.has_value());

    fiber::quic::QuicReadCursor in(buf.data(), out.offset());
    auto parsed = fiber::quic::quic_parse_frame_for_receiver(fiber::quic::QuicConnectionRole::Client,
                                                             fiber::quic::QuicEncryptionLevel::Application, in);

    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->frame.type, fiber::quic::QuicFrameType::HandshakeDone);
    EXPECT_TRUE(parsed->frame.ack_eliciting);
}
