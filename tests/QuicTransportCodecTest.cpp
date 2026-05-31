#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

#include "quic/QuicTransportCodec.h"

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

TEST(QuicTransportCodecTest, CreatesAndParsesCryptoFrame) {
    const std::array<std::uint8_t, 3> payload{0xde, 0xad, 0xbe};
    fiber::quic::QuicFrame frame{};
    frame.type = fiber::quic::QuicFrameType::Crypto;
    frame.u.crypto.offset = 7;
    frame.u.crypto.length = payload.size();
    frame.data = {payload.data(), payload.size()};

    auto expected_len = fiber::quic::quic_create_frame(nullptr, frame);

    ASSERT_TRUE(expected_len.has_value());
    std::array<std::uint8_t, 32> buf{};
    fiber::quic::QuicWriteCursor out(buf.data(), buf.size());
    auto written = fiber::quic::quic_create_frame(&out, frame);

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
}

TEST(QuicTransportCodecTest, ChecksFramePermissionByEncryptionLevel) {
    EXPECT_TRUE(fiber::quic::quic_frame_allowed(fiber::quic::QuicEncryptionLevel::Initial,
                                                fiber::quic::QuicFrameType::Crypto));
    EXPECT_FALSE(fiber::quic::quic_frame_allowed(fiber::quic::QuicEncryptionLevel::Initial,
                                                 fiber::quic::QuicFrameType::Stream));
    EXPECT_FALSE(fiber::quic::quic_frame_allowed(fiber::quic::QuicEncryptionLevel::EarlyData,
                                                 fiber::quic::QuicFrameType::Ack));
    EXPECT_TRUE(fiber::quic::quic_frame_allowed(fiber::quic::QuicEncryptionLevel::Application,
                                                fiber::quic::QuicFrameType::PathResponse));
}
