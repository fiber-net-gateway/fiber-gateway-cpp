#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>

#define private public
#include "quic/QuicStreamTable.h"
#undef private

namespace {

std::array<std::uint64_t, 3> find_colliding_stream_ids(std::size_t bucket_count) {
    std::array<std::uint64_t, 3> out{};
    const std::size_t target_bucket = fiber::quic::QuicStreamTable::hash_stream_id(0) & (bucket_count - 1U);
    std::size_t found = 0;

    for (std::uint64_t stream_id = 0; stream_id < 100000 && found < out.size(); ++stream_id) {
        if ((fiber::quic::QuicStreamTable::hash_stream_id(stream_id) & (bucket_count - 1U)) == target_bucket) {
            out[found++] = stream_id;
        }
    }

    EXPECT_EQ(found, out.size());
    return out;
}

} // namespace

TEST(QuicStreamTest, ExposesStreamIdTypeAndSequence) {
    fiber::quic::QuicStream client_bidi(0);
    fiber::quic::QuicStream server_uni(3);
    fiber::quic::QuicStream later_client_bidi(20);

    EXPECT_EQ(client_bidi.stream_id(), 0U);
    EXPECT_EQ(client_bidi.sequence(), 0U);
    EXPECT_EQ(client_bidi.type(), fiber::quic::QuicStreamType::Bidirectional);
    EXPECT_TRUE(client_bidi.bidirectional());
    EXPECT_FALSE(client_bidi.unidirectional());

    EXPECT_EQ(server_uni.sequence(), 0U);
    EXPECT_EQ(server_uni.type(), fiber::quic::QuicStreamType::Unidirectional);
    EXPECT_FALSE(server_uni.bidirectional());
    EXPECT_TRUE(server_uni.unidirectional());

    EXPECT_EQ(later_client_bidi.sequence(), 5U);
}

TEST(QuicStreamTableTest, InitializesGrowableCapacityFromInitialStreamCapacity) {
    fiber::quic::QuicStreamTable table;

    ASSERT_TRUE(table.init(3));
    EXPECT_EQ(table.bucket_count(), 8U);
    EXPECT_TRUE(table.empty());
}

TEST(QuicStreamTableTest, InsertsFindsAndRejectsDuplicateStreamIds) {
    fiber::quic::QuicStreamTable table;
    ASSERT_TRUE(table.init(4));

    fiber::quic::QuicStream stream0(0);
    fiber::quic::QuicStream stream4(4);
    fiber::quic::QuicStream duplicate0(0);

    EXPECT_TRUE(table.insert(stream0));
    EXPECT_TRUE(table.insert(stream4));
    EXPECT_FALSE(table.insert(duplicate0));
    EXPECT_EQ(table.size(), 2U);
    EXPECT_EQ(table.find(0), &stream0);
    EXPECT_EQ(table.find(4), &stream4);
    EXPECT_EQ(table.find(8), nullptr);
}

TEST(QuicStreamTableTest, GrowsWhenLoadFactorWouldExceedHalf) {
    fiber::quic::QuicStreamTable table;
    ASSERT_TRUE(table.init(1));

    fiber::quic::QuicStream stream0(0);
    fiber::quic::QuicStream stream4(4);
    fiber::quic::QuicStream stream8(8);
    fiber::quic::QuicStream stream12(12);
    fiber::quic::QuicStream stream16(16);
    const std::size_t initial_bucket_count = table.bucket_count();

    EXPECT_TRUE(table.insert(stream0));
    EXPECT_TRUE(table.insert(stream4));
    EXPECT_TRUE(table.insert(stream8));
    EXPECT_TRUE(table.insert(stream12));
    EXPECT_TRUE(table.insert(stream16));

    EXPECT_GT(table.bucket_count(), initial_bucket_count);
    EXPECT_EQ(table.find(0), &stream0);
    EXPECT_EQ(table.find(4), &stream4);
    EXPECT_EQ(table.find(8), &stream8);
    EXPECT_EQ(table.find(12), &stream12);
    EXPECT_EQ(table.find(16), &stream16);
}

TEST(QuicStreamTableTest, EraseKeepsLaterCollisionsReachable) {
    fiber::quic::QuicStreamTable table;
    ASSERT_TRUE(table.init(4));

    const auto ids = find_colliding_stream_ids(table.bucket_count());
    fiber::quic::QuicStream stream_a(ids[0]);
    fiber::quic::QuicStream stream_b(ids[1]);
    fiber::quic::QuicStream stream_c(ids[2]);

    ASSERT_TRUE(table.insert(stream_a));
    ASSERT_TRUE(table.insert(stream_b));
    ASSERT_TRUE(table.insert(stream_c));

    EXPECT_EQ(table.erase(stream_a.stream_id()), &stream_a);
    EXPECT_EQ(table.find(stream_b.stream_id()), &stream_b);
    EXPECT_EQ(table.find(stream_c.stream_id()), &stream_c);

    EXPECT_EQ(table.erase(stream_b.stream_id()), &stream_b);
    EXPECT_EQ(table.find(stream_c.stream_id()), &stream_c);
    EXPECT_EQ(table.size(), 1U);
}

TEST(QuicStreamTableTest, SupportsLargeStreamIds) {
    fiber::quic::QuicStreamTable table;
    ASSERT_TRUE(table.init(2));

    constexpr std::uint64_t large_stream_id = (1ULL << 62U) - 4U;
    fiber::quic::QuicStream stream(large_stream_id);

    EXPECT_TRUE(table.insert(stream));
    EXPECT_EQ(table.find(large_stream_id), &stream);
    EXPECT_EQ(table.erase(large_stream_id), &stream);
    EXPECT_TRUE(table.empty());
}
