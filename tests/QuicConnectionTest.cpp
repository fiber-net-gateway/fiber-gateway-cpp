#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "quic/QuicConnection.h"
#include "quic/QuicProtocol.h"
#include "quic/QuicTransportCodec.h"

TEST(QuicConnectionTest, BuildsConnectionIdFromBytes) {
    const std::array<std::uint8_t, 4> bytes{0x01, 0x02, 0x03, 0x04};

    auto conn_id = fiber::quic::QuicConnectionId::from_bytes(bytes.data(), bytes.size());

    ASSERT_TRUE(conn_id.has_value());
    EXPECT_EQ(conn_id->size(), bytes.size());
    EXPECT_EQ(conn_id->data()[0], 0x01);
    EXPECT_EQ(conn_id->data()[3], 0x04);
}

TEST(QuicConnectionTest, AllocatesClientInitiatedStreamIds) {
    fiber::quic::QuicConnection::Options options{};
    options.role = fiber::quic::QuicConnectionRole::Client;
    options.max_local_bidirectional_streams = 2;
    fiber::quic::QuicConnection conn(options);

    auto first = conn.next_local_stream_id(fiber::quic::QuicStreamType::Bidirectional);
    auto second = conn.next_local_stream_id(fiber::quic::QuicStreamType::Bidirectional);
    auto third = conn.next_local_stream_id(fiber::quic::QuicStreamType::Bidirectional);

    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*first, 0U);
    EXPECT_EQ(*second, 4U);
    EXPECT_FALSE(third.has_value());
}

TEST(QuicConnectionTest, AllocatesServerInitiatedUnidirectionalStreamIds) {
    fiber::quic::QuicConnection::Options options{};
    options.role = fiber::quic::QuicConnectionRole::Server;
    fiber::quic::QuicConnection conn(options);

    auto stream_id = conn.next_local_stream_id(fiber::quic::QuicStreamType::Unidirectional);

    ASSERT_TRUE(stream_id.has_value());
    EXPECT_EQ(*stream_id, 3U);
    EXPECT_TRUE(fiber::quic::QuicConnection::is_unidirectional_stream(*stream_id));
    EXPECT_TRUE(conn.is_local_stream(*stream_id));
}

TEST(QuicConnectionTest, InitializesThreePacketNumberSpaces) {
    fiber::quic::QuicConnection conn(fiber::quic::QuicConnection::Options{});

    auto &initial = conn.packet_number_space(fiber::quic::QuicEncryptionLevel::Initial);
    auto &handshake = conn.packet_number_space(fiber::quic::QuicEncryptionLevel::Handshake);
    auto &application = conn.packet_number_space(fiber::quic::QuicEncryptionLevel::Application);

    EXPECT_EQ(initial.level, fiber::quic::QuicEncryptionLevel::Initial);
    EXPECT_EQ(handshake.level, fiber::quic::QuicEncryptionLevel::Handshake);
    EXPECT_EQ(application.level, fiber::quic::QuicEncryptionLevel::Application);
    EXPECT_EQ(initial.next_packet_number, 0U);
    EXPECT_EQ(handshake.next_packet_number, 0U);
    EXPECT_EQ(application.next_packet_number, 0U);
    EXPECT_EQ(initial.largest_received_packet_number, fiber::quic::kUnsetPacketNumber);
    EXPECT_EQ(handshake.largest_acked_packet_number, fiber::quic::kUnsetPacketNumber);
    EXPECT_EQ(application.pending_ack, fiber::quic::kUnsetPacketNumber);
}

TEST(QuicConnectionTest, MapsEarlyDataToApplicationPacketNumberSpace) {
    fiber::quic::QuicConnection conn(fiber::quic::QuicConnection::Options{});

    auto &early = conn.packet_number_space(fiber::quic::QuicEncryptionLevel::EarlyData);
    auto &application = conn.packet_number_space(fiber::quic::QuicEncryptionLevel::Application);

    EXPECT_EQ(&early, &application);
    EXPECT_EQ(fiber::quic::QuicConnection::packet_number_space_index(fiber::quic::QuicEncryptionLevel::Initial), 0U);
    EXPECT_EQ(fiber::quic::QuicConnection::packet_number_space_index(fiber::quic::QuicEncryptionLevel::Handshake),
              1U);
    EXPECT_EQ(fiber::quic::QuicConnection::packet_number_space_index(fiber::quic::QuicEncryptionLevel::EarlyData),
              2U);
    EXPECT_EQ(fiber::quic::QuicConnection::packet_number_space_index(fiber::quic::QuicEncryptionLevel::Application),
              2U);
}

TEST(QuicConnectionTest, AdvancesPacketNumbersIndependentlyPerSpace) {
    fiber::quic::QuicConnection conn(fiber::quic::QuicConnection::Options{});

    auto &initial = conn.packet_number_space(fiber::quic::QuicEncryptionLevel::Initial);
    auto &handshake = conn.packet_number_space(fiber::quic::QuicEncryptionLevel::Handshake);

    EXPECT_EQ(fiber::quic::quic_use_next_packet_number(initial), 0U);
    EXPECT_EQ(fiber::quic::quic_use_next_packet_number(initial), 1U);
    EXPECT_EQ(fiber::quic::quic_use_next_packet_number(handshake), 0U);
    EXPECT_EQ(initial.next_packet_number, 2U);
    EXPECT_EQ(handshake.next_packet_number, 1U);
}

TEST(QuicConnectionTest, QueuesFramesIntrusively) {
    fiber::quic::QuicConnection conn(fiber::quic::QuicConnection::Options{});
    auto &space = conn.packet_number_space(fiber::quic::QuicEncryptionLevel::Initial);
    fiber::quic::QuicFrame first{};
    fiber::quic::QuicFrame second{};

    space.pending_frames.push_back(first);
    space.pending_frames.push_back(second);

    EXPECT_FALSE(space.pending_frames.empty());
    EXPECT_EQ(space.pending_frames.front(), &first);
    EXPECT_EQ(space.pending_frames.back(), &second);

    space.pending_frames.erase(first);

    EXPECT_EQ(space.pending_frames.front(), &second);
    EXPECT_EQ(space.pending_frames.back(), &second);
}
