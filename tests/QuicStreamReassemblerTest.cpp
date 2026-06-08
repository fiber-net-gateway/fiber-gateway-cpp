#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <string>
#include <string_view>

#include "quic/QuicStreamReassembler.h"

namespace {

fiber::quic::QuicSlice slice_of(std::string_view value) {
    return {reinterpret_cast<const std::uint8_t *>(value.data()), value.size()};
}

std::string chain_to_string(const fiber::mem::IoBufChain &chain) {
    std::array<iovec, 16> iov{};
    int count = chain.fill_write_iov(iov.data(), static_cast<int>(iov.size()));
    std::string out;
    for (int i = 0; i < count; ++i) {
        out.append(static_cast<const char *>(iov[i].iov_base), iov[i].iov_len);
    }
    return out;
}

TEST(QuicStreamReassemblerTest, OutOfOrderDataMergesSameStorageExtentsAndTakesInOrder) {
    fiber::quic::QuicRecvExtentPool pool;
    fiber::quic::QuicStreamReassembler reassembler(pool);

    auto first = reassembler.insert(4, slice_of("ef"));
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(*first, 2u);
    EXPECT_EQ(reassembler.active_extent_count(), 1u);
    EXPECT_EQ(reassembler.active_block_count(), 1u);

    fiber::mem::IoBufChain out;
    auto early_take = reassembler.take(16, out);
    ASSERT_TRUE(early_take.has_value());
    EXPECT_EQ(*early_take, 0u);

    auto second = reassembler.insert(2, slice_of("cd"));
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*second, 2u);
    EXPECT_EQ(reassembler.active_extent_count(), 1u);

    auto third = reassembler.insert(0, slice_of("ab"));
    ASSERT_TRUE(third.has_value());
    EXPECT_EQ(*third, 2u);
    EXPECT_EQ(reassembler.active_extent_count(), 1u);
    EXPECT_EQ(reassembler.buffered_bytes(), 6u);

    auto taken = reassembler.take(16, out);
    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(*taken, 6u);
    EXPECT_EQ(chain_to_string(out), "abcdef");
    EXPECT_EQ(reassembler.next_read_offset(), 6u);
    EXPECT_EQ(reassembler.active_extent_count(), 0u);
    EXPECT_EQ(reassembler.active_block_count(), 0u);
    EXPECT_GE(pool.cached_count(), 1u);
}

TEST(QuicStreamReassemblerTest, OverlappingInsertCopiesOnlyMissingPrefix) {
    fiber::quic::QuicRecvExtentPool pool;
    fiber::quic::QuicStreamReassembler reassembler(pool);

    auto first = reassembler.insert(2, slice_of("cdef"));
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(*first, 4u);

    auto second = reassembler.insert(0, slice_of("abcd"));
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*second, 2u);
    EXPECT_EQ(reassembler.active_extent_count(), 1u);
    EXPECT_EQ(reassembler.buffered_bytes(), 6u);

    fiber::mem::IoBufChain out;
    auto taken = reassembler.take(16, out);
    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(*taken, 6u);
    EXPECT_EQ(chain_to_string(out), "abcdef");
}

TEST(QuicStreamReassemblerTest, DeliveredDuplicateIsIgnored) {
    fiber::quic::QuicRecvExtentPool pool;
    fiber::quic::QuicStreamReassembler reassembler(pool);

    ASSERT_TRUE(reassembler.insert(0, slice_of("abc")).has_value());

    fiber::mem::IoBufChain out;
    auto taken = reassembler.take(3, out);
    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(*taken, 3u);
    EXPECT_EQ(chain_to_string(out), "abc");

    auto duplicate = reassembler.insert(0, slice_of("abc"));
    ASSERT_TRUE(duplicate.has_value());
    EXPECT_EQ(*duplicate, 0u);

    auto next = reassembler.insert(3, slice_of("de"));
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(*next, 2u);

    fiber::mem::IoBufChain tail;
    taken = reassembler.take(8, tail);
    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(*taken, 2u);
    EXPECT_EQ(chain_to_string(tail), "de");
}

TEST(QuicStreamReassemblerTest, FinalSizeMustRemainConsistent) {
    fiber::quic::QuicRecvExtentPool pool;
    fiber::quic::QuicStreamReassembler reassembler(pool);

    auto fin = reassembler.insert(0, slice_of("abc"), true);
    ASSERT_TRUE(fin.has_value());
    EXPECT_TRUE(reassembler.has_final_size());
    EXPECT_EQ(reassembler.final_size(), 3u);

    auto mismatched_fin = reassembler.insert(0, slice_of("abcd"), true);
    EXPECT_FALSE(mismatched_fin.has_value());
}

} // namespace
