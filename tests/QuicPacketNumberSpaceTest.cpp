#include <gtest/gtest.h>

#include "quic/QuicPacketNumberSpace.h"
#include "quic/QuicProtocol.h"
#include "quic/QuicTransportCodec.h"

TEST(QuicPacketNumberSpaceTest, RecordsEcnCountersForReceivedPackets) {
    fiber::quic::QuicPacketNumberSpace space{};
    space.reset(fiber::quic::QuicEncryptionLevel::Application);

    space.on_packet_received(1, fiber::quic::QuicTime{1}, true, fiber::net::UdpEcn::Ect0);
    space.on_packet_received(2, fiber::quic::QuicTime{2}, true, fiber::net::UdpEcn::Ect1);
    space.on_packet_received(3, fiber::quic::QuicTime{3}, true, fiber::net::UdpEcn::Ce);
    space.on_packet_received(4, fiber::quic::QuicTime{4}, true, fiber::net::UdpEcn::NonEct);
    space.on_packet_received(5, fiber::quic::QuicTime{5}, true, fiber::net::UdpEcn::Unspecified);

    EXPECT_EQ(space.ecn_counters.ect0, 1U);
    EXPECT_EQ(space.ecn_counters.ect1, 1U);
    EXPECT_EQ(space.ecn_counters.ce, 1U);
}

TEST(QuicPacketNumberSpaceTest, PreparesAckEcnFrameWhenCountersArePresent) {
    fiber::quic::QuicOutputFrame frame{};
    fiber::quic::QuicEcnCounters counters{};
    counters.ect0 = 2;
    counters.ce = 1;

    auto prepared = fiber::quic::quic_prepare_ack_frame(frame, 9, 3, 0, 0, nullptr, counters);

    ASSERT_TRUE(prepared.has_value());
    EXPECT_EQ(frame.type, fiber::quic::QuicFrameType::AckEcn);
    EXPECT_EQ(frame.u.ack.largest, 9U);
    EXPECT_EQ(frame.u.ack.delay, 3U);
    EXPECT_EQ(frame.u.ack.ect0, 2U);
    EXPECT_EQ(frame.u.ack.ect1, 0U);
    EXPECT_EQ(frame.u.ack.ce, 1U);

    auto encoded_len = fiber::quic::quic_output_frame_encoded_len(frame);
    ASSERT_TRUE(encoded_len.has_value());
    EXPECT_GT(*encoded_len, 0U);

    fiber::quic::quic_output_frame_release_data(frame);
}

TEST(QuicPacketNumberSpaceTest, PreparesPlainAckFrameWhenCountersAreEmpty) {
    fiber::quic::QuicOutputFrame frame{};
    fiber::quic::QuicEcnCounters counters{};

    auto prepared = fiber::quic::quic_prepare_ack_frame(frame, 7, 0, 0, 0, nullptr, counters);

    ASSERT_TRUE(prepared.has_value());
    EXPECT_EQ(frame.type, fiber::quic::QuicFrameType::Ack);
    EXPECT_EQ(frame.u.ack.largest, 7U);
}

TEST(QuicPacketNumberSpaceTest, DropAckRangesDoesNotResetEcnCounters) {
    fiber::quic::QuicPacketNumberSpace space{};
    space.reset(fiber::quic::QuicEncryptionLevel::Application);

    space.on_packet_received(1, fiber::quic::QuicTime{1}, true, fiber::net::UdpEcn::Ect0);
    space.on_packet_received(2, fiber::quic::QuicTime{2}, true, fiber::net::UdpEcn::Ce);
    space.drop_ack_ranges(2);

    EXPECT_EQ(space.ecn_counters.ect0, 1U);
    EXPECT_EQ(space.ecn_counters.ce, 1U);

    space.reset(fiber::quic::QuicEncryptionLevel::Application);
    EXPECT_EQ(space.ecn_counters.ect0, 0U);
    EXPECT_EQ(space.ecn_counters.ect1, 0U);
    EXPECT_EQ(space.ecn_counters.ce, 0U);
}

TEST(QuicPacketNumberSpaceTest, ForcedAckUsesEcnCountersBeforeOverflowPacket) {
    fiber::quic::QuicPacketNumberSpace space{};
    space.reset(fiber::quic::QuicEncryptionLevel::Application);

    space.on_packet_received(100, fiber::quic::QuicTime{100}, true, fiber::net::UdpEcn::Ect0);
    for (std::uint64_t pn = 98; space.ack_range_count < fiber::quic::kQuicMaxAckRanges; pn -= 2) {
        space.on_packet_received(pn, fiber::quic::QuicTime{100}, false, fiber::net::UdpEcn::NonEct);
    }

    space.on_packet_received(200, fiber::quic::QuicTime{200}, true, fiber::net::UdpEcn::Ect1);

    ASSERT_NE(space.pending_frames.front(), nullptr);
    EXPECT_EQ(space.pending_frames.front()->type, fiber::quic::QuicFrameType::AckEcn);
    EXPECT_EQ(space.pending_frames.front()->u.ack.ect0, 1U);
    EXPECT_EQ(space.pending_frames.front()->u.ack.ect1, 0U);
    EXPECT_EQ(space.ecn_counters.ect1, 1U);
}
