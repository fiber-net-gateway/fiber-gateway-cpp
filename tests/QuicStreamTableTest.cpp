#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>

#define private public
#include <fiber/quic/QuicStreamTable.h>
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

void destroy_heap_stream(void *, fiber::quic::QuicStream &stream) noexcept { delete &stream; }

void noop_destroy_stream(void *, fiber::quic::QuicStream &) noexcept {}

fiber::quic::QuicStream::Lease make_stream(std::uint64_t stream_id, fiber::mem::IoBufNodePool &) {
    auto *stream = new fiber::quic::QuicStream(nullptr, destroy_heap_stream);
    stream->stream_id_ = stream_id;
    return fiber::quic::QuicStream::Lease::adopt(stream);
}

} // namespace

TEST(QuicStreamTest, ExposesStreamIdTypeAndSequence) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicStream client_bidi(nullptr, noop_destroy_stream);
    fiber::quic::QuicStream server_uni(nullptr, noop_destroy_stream);
    fiber::quic::QuicStream later_client_bidi(nullptr, noop_destroy_stream);
    client_bidi.stream_id_ = 0;
    server_uni.stream_id_ = 3;
    later_client_bidi.stream_id_ = 20;

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

    fiber::mem::IoBufNodePool pool;
    auto stream0 = make_stream(0, pool);
    auto stream4 = make_stream(4, pool);
    auto duplicate0 = make_stream(0, pool);
    auto *stream0_ptr = stream0.get();
    auto *stream4_ptr = stream4.get();

    EXPECT_TRUE(table.insert(std::move(stream0)));
    EXPECT_TRUE(table.insert(std::move(stream4)));
    EXPECT_FALSE(table.insert(std::move(duplicate0)));
    EXPECT_EQ(table.size(), 2U);
    EXPECT_EQ(table.find(0), stream0_ptr);
    EXPECT_EQ(table.find(4), stream4_ptr);
    EXPECT_EQ(table.find(8), nullptr);
}

TEST(QuicStreamTableTest, GrowsWhenLoadFactorWouldExceedHalf) {
    fiber::quic::QuicStreamTable table;
    ASSERT_TRUE(table.init(1));

    fiber::mem::IoBufNodePool pool;
    auto stream0 = make_stream(0, pool);
    auto stream4 = make_stream(4, pool);
    auto stream8 = make_stream(8, pool);
    auto stream12 = make_stream(12, pool);
    auto stream16 = make_stream(16, pool);
    auto *stream0_ptr = stream0.get();
    auto *stream4_ptr = stream4.get();
    auto *stream8_ptr = stream8.get();
    auto *stream12_ptr = stream12.get();
    auto *stream16_ptr = stream16.get();
    const std::size_t initial_bucket_count = table.bucket_count();

    EXPECT_TRUE(table.insert(std::move(stream0)));
    EXPECT_TRUE(table.insert(std::move(stream4)));
    EXPECT_TRUE(table.insert(std::move(stream8)));
    EXPECT_TRUE(table.insert(std::move(stream12)));
    EXPECT_TRUE(table.insert(std::move(stream16)));

    EXPECT_GT(table.bucket_count(), initial_bucket_count);
    EXPECT_EQ(table.find(0), stream0_ptr);
    EXPECT_EQ(table.find(4), stream4_ptr);
    EXPECT_EQ(table.find(8), stream8_ptr);
    EXPECT_EQ(table.find(12), stream12_ptr);
    EXPECT_EQ(table.find(16), stream16_ptr);
}

TEST(QuicStreamTableTest, EraseKeepsLaterCollisionsReachable) {
    fiber::quic::QuicStreamTable table;
    ASSERT_TRUE(table.init(4));

    fiber::mem::IoBufNodePool pool;
    const auto ids = find_colliding_stream_ids(table.bucket_count());
    auto stream_a = make_stream(ids[0], pool);
    auto stream_b = make_stream(ids[1], pool);
    auto stream_c = make_stream(ids[2], pool);
    auto *stream_a_ptr = stream_a.get();
    auto *stream_b_ptr = stream_b.get();
    auto *stream_c_ptr = stream_c.get();

    ASSERT_TRUE(table.insert(std::move(stream_a)));
    ASSERT_TRUE(table.insert(std::move(stream_b)));
    ASSERT_TRUE(table.insert(std::move(stream_c)));

    auto erased_a = table.erase(stream_a_ptr->stream_id());
    EXPECT_EQ(erased_a.get(), stream_a_ptr);
    EXPECT_EQ(table.find(stream_b_ptr->stream_id()), stream_b_ptr);
    EXPECT_EQ(table.find(stream_c_ptr->stream_id()), stream_c_ptr);

    auto erased_b = table.erase(stream_b_ptr->stream_id());
    EXPECT_EQ(erased_b.get(), stream_b_ptr);
    EXPECT_EQ(table.find(stream_c_ptr->stream_id()), stream_c_ptr);
    EXPECT_EQ(table.size(), 1U);
}

TEST(QuicStreamTableTest, SupportsLargeStreamIds) {
    fiber::quic::QuicStreamTable table;
    ASSERT_TRUE(table.init(2));

    fiber::mem::IoBufNodePool pool;
    constexpr std::uint64_t large_stream_id = (1ULL << 62U) - 4U;
    auto stream = make_stream(large_stream_id, pool);
    auto *stream_ptr = stream.get();

    EXPECT_TRUE(table.insert(std::move(stream)));
    EXPECT_EQ(table.find(large_stream_id), stream_ptr);
    auto erased = table.erase(large_stream_id);
    EXPECT_EQ(erased.get(), stream_ptr);
    EXPECT_TRUE(table.empty());
}
