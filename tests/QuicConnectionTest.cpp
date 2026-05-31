#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "quic/QuicConnection.h"

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
