#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

#include <fiber/quic/QuicDataReassembler.h>

namespace {

fiber::mem::IoBuf slice_of(std::string_view value) {
    fiber::mem::IoBuf buf = fiber::mem::IoBuf::allocate(value.size());
    if (buf) {
        std::memcpy(buf.writable_data(), value.data(), value.size());
        buf.commit(value.size());
    }
    return buf;
}

fiber::mem::IoBuf trackable_slice_of(std::string_view value, std::size_t capacity) {
    fiber::mem::IoBuf buf = fiber::mem::IoBuf::allocate_trackable(capacity);
    if (buf && value.size() <= buf.writable()) {
        std::memcpy(buf.writable_data(), value.data(), value.size());
        buf.commit(value.size());
    }
    return buf;
}

std::string chain_to_string(const fiber::mem::IoBufChain &chain) {
    std::array<iovec, 32> iov{};
    int count = chain.fill_write_iov(iov.data(), static_cast<int>(iov.size()));
    std::string out;
    for (int i = 0; i < count; ++i) {
        out.append(static_cast<const char *>(iov[i].iov_base), iov[i].iov_len);
    }
    return out;
}

} // namespace

TEST(QuicDataReassemblerTest, SequentialInsertCanBeTakenImmediately) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicDataReassembler reassembler;
    reassembler.init(pool);

    auto inserted = reassembler.insert(0, slice_of("abc"));
    ASSERT_TRUE(inserted.has_value());
    EXPECT_EQ(*inserted, 3U);

    fiber::mem::IoBufChain out;
    EXPECT_FALSE(out.bound());
    auto taken = reassembler.take_contiguous(out);
    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(*taken, 3U);
    EXPECT_TRUE(out.bound());
    EXPECT_EQ(&out.node_pool(), &pool);
    EXPECT_EQ(chain_to_string(out), "abc");
    EXPECT_EQ(reassembler.next_offset(), 3U);
    EXPECT_EQ(reassembler.buffered_bytes(), 0U);
}

TEST(QuicDataReassemblerTest, OutOfOrderDataWaitsForGapThenTakesAllContiguousBytes) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicDataReassembler reassembler;
    reassembler.init(pool);

    auto inserted = reassembler.insert(3, slice_of("def"));
    ASSERT_TRUE(inserted.has_value());
    EXPECT_EQ(*inserted, 3U);

    fiber::mem::IoBufChain out(pool);
    auto taken = reassembler.take_contiguous(out);
    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(*taken, 0U);
    EXPECT_TRUE(out.empty());

    inserted = reassembler.insert(0, slice_of("abc"));
    ASSERT_TRUE(inserted.has_value());
    EXPECT_EQ(*inserted, 3U);

    taken = reassembler.take_contiguous(out);
    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(*taken, 6U);
    EXPECT_EQ(chain_to_string(out), "abcdef");
    EXPECT_EQ(reassembler.next_offset(), 6U);
}

TEST(QuicDataReassemblerTest, OverlapAcrossGapRetainsOnlyMissingBytes) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicDataReassembler reassembler;
    reassembler.init(pool);

    auto inserted = reassembler.insert(2, slice_of("cdef"));
    ASSERT_TRUE(inserted.has_value());
    EXPECT_EQ(*inserted, 4U);

    inserted = reassembler.insert(0, slice_of("abcd"));
    ASSERT_TRUE(inserted.has_value());
    EXPECT_EQ(*inserted, 2U);

    fiber::mem::IoBufChain out(pool);
    auto taken = reassembler.take_contiguous(out);
    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(*taken, 6U);
    EXPECT_EQ(chain_to_string(out), "abcdef");
    EXPECT_EQ(reassembler.buffered_bytes(), 0U);
}

TEST(QuicDataReassemblerTest, OnePacketCanFillMultipleHolesWithoutCopyingStorage) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicDataReassembler reassembler(pool);

    ASSERT_TRUE(reassembler.insert(2, slice_of("cd")).has_value());
    fiber::mem::IoBuf source = slice_of("abcdef");
    fiber::mem::IoBuf storage_witness = source;
    auto inserted = reassembler.insert(0, std::move(source));
    ASSERT_TRUE(inserted.has_value());
    EXPECT_EQ(*inserted, 4U);
    EXPECT_EQ(reassembler.active_extent_count(), 3U);

    fiber::mem::IoBufChain out(pool);
    auto taken = reassembler.take_contiguous(out);
    ASSERT_TRUE(taken.has_value());
    ASSERT_NE(out.front(), nullptr);
    ASSERT_NE(out.back(), nullptr);
    EXPECT_TRUE(out.front()->same_storage(storage_witness));
    EXPECT_TRUE(out.back()->same_storage(storage_witness));
    EXPECT_EQ(chain_to_string(out), "abcdef");
}

TEST(QuicDataReassemblerTest, DeliveredDuplicateIsIgnored) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicDataReassembler reassembler;
    reassembler.init(pool);

    ASSERT_TRUE(reassembler.insert(0, slice_of("abc")).has_value());
    fiber::mem::IoBufChain out(pool);
    ASSERT_TRUE(reassembler.take_contiguous(out).has_value());
    out.clear();

    auto duplicate = reassembler.insert(0, slice_of("abc"));
    ASSERT_TRUE(duplicate.has_value());
    EXPECT_EQ(*duplicate, 0U);

    auto inserted = reassembler.insert(3, slice_of("de"));
    ASSERT_TRUE(inserted.has_value());
    EXPECT_EQ(*inserted, 2U);

    auto taken = reassembler.take_contiguous(out);
    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(*taken, 2U);
    EXPECT_EQ(chain_to_string(out), "de");
    EXPECT_EQ(reassembler.next_offset(), 5U);
}

TEST(QuicDataReassemblerTest, BufferedDuplicateIsIgnored) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicDataReassembler reassembler;
    reassembler.init(pool);

    auto first = reassembler.insert(4, slice_of("ef"));
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(*first, 2U);

    auto duplicate = reassembler.insert(4, slice_of("ef"));
    ASSERT_TRUE(duplicate.has_value());
    EXPECT_EQ(*duplicate, 0U);
    EXPECT_EQ(reassembler.buffered_bytes(), 2U);
}

TEST(QuicDataReassemblerTest, FrameCrossingFormerBlockBoundaryStaysInOneExtentAndIsTakenInOrder) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicDataReassembler reassembler;
    reassembler.init(pool);

    constexpr std::size_t block = 4 * 1024;
    std::string payload(32, 'x');
    payload.replace(0, 4, "abcd");
    payload.replace(payload.size() - 4, 4, "wxyz");

    auto inserted = reassembler.insert(block - 16, slice_of(payload));
    ASSERT_TRUE(inserted.has_value());
    EXPECT_EQ(*inserted, payload.size());
    EXPECT_EQ(reassembler.active_extent_count(), 1U);

    std::string prefix(block - 16, 'p');
    inserted = reassembler.insert(0, slice_of(prefix));
    ASSERT_TRUE(inserted.has_value());
    EXPECT_EQ(*inserted, prefix.size());

    fiber::mem::IoBufChain out(pool);
    auto taken = reassembler.take_contiguous(out);
    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(*taken, block + 16U);

    std::string expected = prefix + payload;
    EXPECT_EQ(chain_to_string(out), expected);
    EXPECT_EQ(reassembler.next_offset(), expected.size());
}

TEST(QuicDataReassemblerTest, BufferLimitRejectsAdditionalBufferedBytesWithoutConsumingExistingData) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicDataReassembler reassembler;
    reassembler.init(pool, {.buffer_limit = 8});

    auto inserted = reassembler.insert(4, slice_of("abcdefgh"));
    ASSERT_TRUE(inserted.has_value());
    EXPECT_EQ(*inserted, 8U);
    EXPECT_EQ(reassembler.buffered_bytes(), 8U);

    inserted = reassembler.insert(12, slice_of("x"));
    ASSERT_FALSE(inserted.has_value());
    EXPECT_EQ(inserted.error(), fiber::common::IoErr::MessageTooLarge);
    EXPECT_EQ(reassembler.buffered_bytes(), 8U);

    inserted = reassembler.insert(0, slice_of("0123"));
    ASSERT_TRUE(inserted.has_value());

    fiber::mem::IoBufChain out(pool);
    auto taken = reassembler.take_contiguous(out);
    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(*taken, 12U);
    EXPECT_EQ(chain_to_string(out), "0123abcdefgh");
}

TEST(QuicDataReassemblerTest, ExtentLimitRejectsInsertWithoutChangingBufferedData) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicDataReassembler reassembler(
            pool, {
                          .buffer_limit = 64,
                          .max_active_extents = 1,
                          .buffer_accounting = fiber::quic::QuicDataReassembler::BufferAccounting::AllRetained,
                  });

    ASSERT_TRUE(reassembler.insert(4, slice_of("a")).has_value());
    auto rejected = reassembler.insert(6, slice_of("b"));
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error(), fiber::common::IoErr::MessageTooLarge);
    EXPECT_EQ(reassembler.buffered_bytes(), 1U);
    EXPECT_EQ(reassembler.active_extent_count(), 1U);
}

TEST(QuicDataReassemblerTest, ClearDropsBufferedDataAndResetsOffset) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicDataReassembler reassembler;
    reassembler.init(pool);

    ASSERT_TRUE(reassembler.insert(4, slice_of("late")).has_value());
    EXPECT_EQ(reassembler.buffered_bytes(), 4U);
    reassembler.clear();

    EXPECT_EQ(reassembler.buffered_bytes(), 0U);
    EXPECT_EQ(reassembler.next_offset(), 0U);
    EXPECT_EQ(reassembler.active_extent_count(), 0U);
}

TEST(QuicDataReassemblerTest, SequentialDataRetainsOriginalIoBufStorage) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicDataReassembler reassembler(pool);

    fiber::mem::IoBuf source = slice_of("zero-copy");
    fiber::mem::IoBuf storage_witness = source;
    ASSERT_TRUE(reassembler.insert(0, std::move(source)).has_value());

    fiber::mem::IoBufChain out(pool);
    auto taken = reassembler.take_contiguous(out);
    ASSERT_TRUE(taken.has_value());
    ASSERT_NE(out.front(), nullptr);
    EXPECT_TRUE(out.front()->same_storage(storage_witness));
    EXPECT_EQ(chain_to_string(out), "zero-copy");
}

TEST(QuicDataReassemblerTest, OneBackingAcrossMultipleHolesCountsCapacityOnce) {
    fiber::mem::IoBufNodePool pool;
    fiber::mem::IoBufStorageBudget budget(128);
    fiber::quic::QuicDataReassembler reassembler(
            pool, {
                          .buffer_limit = 64,
                          .buffer_accounting = fiber::quic::QuicDataReassembler::BufferAccounting::AllRetained,
                          .storage_budget = &budget,
                          .compact_min_backing_capacity = std::numeric_limits<std::size_t>::max(),
                  });

    ASSERT_TRUE(reassembler.insert(2, trackable_slice_of("cd", 2)).has_value());
    EXPECT_EQ(budget.retained_capacity(), 2U);

    fiber::mem::IoBuf source = trackable_slice_of("abcdef", 64);
    auto inserted = reassembler.insert(0, std::move(source));
    ASSERT_TRUE(inserted.has_value());
    EXPECT_EQ(*inserted, 4U);
    EXPECT_EQ(budget.retained_capacity(), 66U);

    fiber::mem::IoBufChain out(pool);
    ASSERT_TRUE(reassembler.take_contiguous(out).has_value());
    EXPECT_EQ(chain_to_string(out), "abcdef");
    EXPECT_EQ(budget.retained_capacity(), 0U);
}

TEST(QuicDataReassemblerTest, SharedBackingAcrossReassemblersCountsOnce) {
    fiber::mem::IoBufNodePool pool;
    fiber::mem::IoBufStorageBudget budget(64);
    fiber::quic::QuicDataReassembler first(
            pool, {.storage_budget = &budget, .compact_min_backing_capacity = std::numeric_limits<std::size_t>::max()});
    fiber::quic::QuicDataReassembler second(
            pool, {.storage_budget = &budget, .compact_min_backing_capacity = std::numeric_limits<std::size_t>::max()});

    fiber::mem::IoBuf packet = trackable_slice_of("ab", 64);
    ASSERT_TRUE(first.insert(4, packet.retain_slice(0, 1)).has_value());
    ASSERT_TRUE(second.insert(8, packet.retain_slice(1, 1)).has_value());
    EXPECT_EQ(budget.retained_capacity(), 64U);

    first.clear();
    EXPECT_EQ(budget.retained_capacity(), 64U);
    second.clear();
    EXPECT_EQ(budget.retained_capacity(), 0U);
}

TEST(QuicDataReassemblerTest, PhysicalBudgetRejectionDoesNotMutateQueue) {
    fiber::mem::IoBufNodePool pool;
    fiber::mem::IoBufStorageBudget budget(16);
    fiber::quic::QuicDataReassembler reassembler(
            pool, {
                          .buffer_limit = 64,
                          .buffer_accounting = fiber::quic::QuicDataReassembler::BufferAccounting::AllRetained,
                          .storage_budget = &budget,
                          .compact_min_backing_capacity = std::numeric_limits<std::size_t>::max(),
                  });

    std::string payload(32, 'x');
    auto rejected = reassembler.insert(1, trackable_slice_of(payload, payload.size()));
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error(), fiber::common::IoErr::NoMem);
    EXPECT_EQ(reassembler.buffered_bytes(), 0U);
    EXPECT_EQ(reassembler.active_extent_count(), 0U);
    EXPECT_EQ(budget.retained_capacity(), 0U);
    EXPECT_EQ(budget.rejected_count(), 1U);
}

TEST(QuicDataReassemblerTest, SmallSliceFromLargeBackingIsCompacted) {
    fiber::mem::IoBufNodePool pool;
    fiber::mem::IoBufStorageBudget budget(64);
    fiber::quic::QuicDataReassembler reassembler(
            pool, {
                          .buffer_limit = 64,
                          .buffer_accounting = fiber::quic::QuicDataReassembler::BufferAccounting::AllRetained,
                          .storage_budget = &budget,
                  });

    fiber::mem::IoBuf packet = trackable_slice_of("x", 64 * 1024);
    auto inserted = reassembler.insert(4, std::move(packet));
    ASSERT_TRUE(inserted.has_value());
    EXPECT_EQ(*inserted, 1U);
    EXPECT_EQ(budget.retained_capacity(), 1U);

    reassembler.clear();
    EXPECT_EQ(budget.retained_capacity(), 0U);
}

TEST(QuicDataReassemblerTest, PartialTakeKeepsBackingChargedUntilExtentLeavesQueue) {
    fiber::mem::IoBufNodePool pool;
    fiber::mem::IoBufStorageBudget budget(32);
    fiber::quic::QuicDataReassembler reassembler(
            pool, {
                          .buffer_limit = 32,
                          .buffer_accounting = fiber::quic::QuicDataReassembler::BufferAccounting::AllRetained,
                          .storage_budget = &budget,
                          .compact_min_backing_capacity = std::numeric_limits<std::size_t>::max(),
                  });

    ASSERT_TRUE(reassembler.insert(0, trackable_slice_of("abcd", 32)).has_value());
    EXPECT_EQ(budget.retained_capacity(), 32U);

    fiber::mem::IoBufChain out(pool);
    auto taken = reassembler.take_contiguous(out, 2);
    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(*taken, 2U);
    EXPECT_EQ(budget.retained_capacity(), 32U);

    taken = reassembler.take_contiguous(out, 2);
    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(*taken, 2U);
    EXPECT_EQ(budget.retained_capacity(), 0U);
}

TEST(QuicDataReassemblerTest, MergingExtentsBalancesStorageReferences) {
    fiber::mem::IoBufNodePool pool;
    fiber::mem::IoBufStorageBudget budget(32);
    fiber::quic::QuicDataReassembler reassembler(
            pool, {.storage_budget = &budget, .compact_min_backing_capacity = std::numeric_limits<std::size_t>::max()});

    fiber::mem::IoBuf packet = trackable_slice_of("abcd", 32);
    ASSERT_TRUE(reassembler.insert(2, packet.retain_slice(0, 2)).has_value());
    ASSERT_TRUE(reassembler.insert(4, packet.retain_slice(2, 2)).has_value());
    EXPECT_EQ(reassembler.active_extent_count(), 1U);
    EXPECT_EQ(budget.retained_capacity(), 32U);

    reassembler.clear();
    EXPECT_EQ(budget.retained_capacity(), 0U);
}
