#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <string>
#include <string_view>

#include "async/Sleep.h"
#include "async/Spawn.h"
#include "event/EventLoopGroup.h"
#include "quic/QuicStreamRecvQueue.h"

namespace fiber::quic {

struct QuicStreamRecvQueueTestAccess {
    static common::IoResult<std::size_t> recv_stream_data(QuicStreamRecvQueue &queue, std::uint64_t offset,
                                                          QuicSlice data, bool fin = false) noexcept {
        return queue.recv_stream_data(offset, data, fin);
    }

    static common::IoResult<void> recv_reset(QuicStreamRecvQueue &queue, std::uint64_t error_code,
                                             std::uint64_t final_size) noexcept {
        return queue.recv_reset(error_code, final_size);
    }
};

} // namespace fiber::quic

namespace {

using DetachedTask = fiber::async::DetachedTask;

constexpr std::uint64_t kStreamRecvBlockSize = 64 * 1024;

struct ReadResult {
    bool ok = false;
    std::size_t value = 0;
    fiber::common::IoErr error = fiber::common::IoErr::None;
    std::string data;
};

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

ReadResult to_read_result(fiber::common::IoResult<std::size_t> result, const fiber::mem::IoBufChain &out) {
    if (result) {
        return {.ok = true, .value = *result, .data = chain_to_string(out)};
    }
    return {.ok = false, .error = result.error()};
}

DetachedTask read_one(fiber::quic::QuicStreamRecvQueue *queue, std::promise<ReadResult> *done) {
    fiber::mem::IoBufChain out(queue->node_pool());
    auto result = co_await queue->take(16, out);
    done->set_value(to_read_result(result, out));
    fiber::event::EventLoop::current().stop();
}

DetachedTask recv_after_delay(fiber::quic::QuicStreamRecvQueue *queue, std::atomic<bool> *waiter_seen) {
    co_await fiber::async::sleep(std::chrono::milliseconds(20));
    waiter_seen->store(queue->has_read_waiter(), std::memory_order_relaxed);
    (void) fiber::quic::QuicStreamRecvQueueTestAccess::recv_stream_data(*queue, 0, slice_of("abc"));
}

} // namespace

TEST(QuicStreamRecvQueueTest, OutOfOrderBytesCountAgainstBufferLimit) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicStreamRecvQueue queue(pool);

    std::string late(48 * 1024, 'x');
    auto inserted = fiber::quic::QuicStreamRecvQueueTestAccess::recv_stream_data(queue, 16 * 1024, slice_of(late));
    ASSERT_TRUE(inserted.has_value());
    EXPECT_EQ(*inserted, late.size());
    EXPECT_EQ(queue.buffered_bytes(), late.size());

    fiber::mem::IoBufChain out(pool);
    auto blocked = queue.try_take(16, out);
    ASSERT_FALSE(blocked.has_value());
    EXPECT_EQ(blocked.error(), fiber::common::IoErr::WouldBlock);

    std::string prefix(16 * 1024, 'p');
    inserted = fiber::quic::QuicStreamRecvQueueTestAccess::recv_stream_data(queue, 0, slice_of(prefix));
    ASSERT_TRUE(inserted.has_value());
    EXPECT_EQ(queue.buffered_bytes(), 64 * 1024U);
}

TEST(QuicStreamRecvQueueTest, RejectsBufferedDataPastLimitWithoutConsumingExistingData) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicStreamRecvQueue queue(pool, {
                                                         .buffer_limit = 64 * 1024,
                                                         .low_water = 16 * 1024,
                                                         .max_stream_data = 128 * 1024,
                                                 });

    std::string block(64 * 1024, 'a');
    auto inserted = fiber::quic::QuicStreamRecvQueueTestAccess::recv_stream_data(queue, 0, slice_of(block));
    ASSERT_TRUE(inserted.has_value());
    EXPECT_EQ(queue.buffered_bytes(), block.size());

    inserted = fiber::quic::QuicStreamRecvQueueTestAccess::recv_stream_data(queue, 64 * 1024, slice_of("x"));
    ASSERT_FALSE(inserted.has_value());
    EXPECT_EQ(inserted.error(), fiber::common::IoErr::MessageTooLarge);
    EXPECT_EQ(queue.buffered_bytes(), block.size());
    EXPECT_EQ(queue.received_end_offset(), block.size());
}

TEST(QuicStreamRecvQueueTest, LowWaterSuggestsNextMaxStreamDataAfterApplicationTakesData) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicStreamRecvQueue queue(pool);

    std::string payload(50 * 1024, 'd');
    ASSERT_TRUE(fiber::quic::QuicStreamRecvQueueTestAccess::recv_stream_data(queue, 0, slice_of(payload)).has_value());

    fiber::mem::IoBufChain out;
    EXPECT_FALSE(out.bound());
    auto taken = queue.try_take(payload.size(), out);
    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(*taken, payload.size());
    EXPECT_TRUE(out.bound());
    EXPECT_EQ(&out.node_pool(), &pool);

    EXPECT_TRUE(queue.should_extend_max_stream_data());
    EXPECT_EQ(queue.next_max_stream_data_limit(), 114 * 1024U);
}

TEST(QuicStreamRecvQueueTest, ResetClearsBufferedDataAndFutureTakeReturnsConnReset) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicStreamRecvQueue queue(pool);

    ASSERT_TRUE(fiber::quic::QuicStreamRecvQueueTestAccess::recv_stream_data(queue, 0, slice_of("abc")).has_value());
    ASSERT_TRUE(fiber::quic::QuicStreamRecvQueueTestAccess::recv_reset(queue, 42, 3).has_value());
    EXPECT_EQ(queue.buffered_bytes(), 0U);
    EXPECT_TRUE(queue.reset_received());
    EXPECT_EQ(queue.reset_error_code(), 42U);
    EXPECT_TRUE(queue.has_final_size());
    EXPECT_EQ(queue.final_size(), 3U);

    fiber::mem::IoBufChain out(pool);
    auto taken = queue.try_take(16, out);
    ASSERT_FALSE(taken.has_value());
    EXPECT_EQ(taken.error(), fiber::common::IoErr::ConnReset);
}

TEST(QuicStreamRecvQueueTest, StopReceivingClearsBufferedDataAndFutureTakeReturnsCanceled) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicStreamRecvQueue queue(pool);

    ASSERT_TRUE(fiber::quic::QuicStreamRecvQueueTestAccess::recv_stream_data(queue, 0, slice_of("abc")).has_value());
    queue.stop_receiving(7);
    EXPECT_EQ(queue.buffered_bytes(), 0U);
    EXPECT_TRUE(queue.stop_sending());
    EXPECT_EQ(queue.stop_error_code(), 7U);

    fiber::mem::IoBufChain out(pool);
    auto taken = queue.try_take(16, out);
    ASSERT_FALSE(taken.has_value());
    EXPECT_EQ(taken.error(), fiber::common::IoErr::Canceled);

    auto ignored = fiber::quic::QuicStreamRecvQueueTestAccess::recv_stream_data(queue, 3, slice_of("def"));
    ASSERT_TRUE(ignored.has_value());
    EXPECT_EQ(*ignored, 0U);
    EXPECT_EQ(queue.buffered_bytes(), 0U);
}

TEST(QuicStreamRecvQueueTest, ResetFinalSizeBecomesReceivedEndOffset) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicStreamRecvQueue queue(pool);

    ASSERT_TRUE(fiber::quic::QuicStreamRecvQueueTestAccess::recv_stream_data(queue, 0, slice_of("abc")).has_value());
    ASSERT_TRUE(fiber::quic::QuicStreamRecvQueueTestAccess::recv_reset(queue, 42, 8).has_value());

    EXPECT_TRUE(queue.reset_received());
    EXPECT_TRUE(queue.has_final_size());
    EXPECT_EQ(queue.received_end_offset(), 8U);
    EXPECT_EQ(queue.final_size(), 8U);
    EXPECT_EQ(queue.buffered_bytes(), 0U);
}

TEST(QuicStreamRecvQueueTest, TakeWaitsUntilReadableDataArrives) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicStreamRecvQueue queue(pool);

    fiber::event::EventLoopGroup group(1);
    std::promise<ReadResult> done;
    auto future = done.get_future();
    std::atomic<bool> waiter_seen{false};

    group.start();
    fiber::async::spawn(group.at(0), [&queue, &done]() { return read_one(&queue, &done); });
    fiber::async::spawn(group.at(0), [&queue, &waiter_seen]() { return recv_after_delay(&queue, &waiter_seen); });

    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "read did not resume after data";
        return;
    }

    ReadResult result = future.get();
    EXPECT_TRUE(waiter_seen.load(std::memory_order_relaxed));
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.value, 3U);
    EXPECT_EQ(result.data, "abc");
    group.join();
}

TEST(QuicStreamRecvQueueTest, OutOfOrderDataMergesSameStorageExtentsAndTakesInOrder) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicStreamRecvQueue queue(pool);

    auto first = fiber::quic::QuicStreamRecvQueueTestAccess::recv_stream_data(queue, 4, slice_of("ef"));
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(*first, 2u);
    EXPECT_EQ(queue.received_end_offset(), 6u);
    EXPECT_EQ(queue.active_extent_count(), 1u);
    EXPECT_EQ(queue.active_block_count(), 1u);

    fiber::mem::IoBufChain out(pool);
    auto early_take = queue.try_take(16, out);
    ASSERT_FALSE(early_take.has_value());
    EXPECT_EQ(early_take.error(), fiber::common::IoErr::WouldBlock);

    auto second = fiber::quic::QuicStreamRecvQueueTestAccess::recv_stream_data(queue, 2, slice_of("cd"));
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*second, 2u);
    EXPECT_EQ(queue.received_end_offset(), 6u);
    EXPECT_EQ(queue.active_extent_count(), 1u);

    auto third = fiber::quic::QuicStreamRecvQueueTestAccess::recv_stream_data(queue, 0, slice_of("ab"));
    ASSERT_TRUE(third.has_value());
    EXPECT_EQ(*third, 2u);
    EXPECT_EQ(queue.received_end_offset(), 6u);
    EXPECT_EQ(queue.active_extent_count(), 1u);
    EXPECT_EQ(queue.buffered_bytes(), 6u);

    auto taken = queue.try_take(16, out);
    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(*taken, 6u);
    EXPECT_EQ(chain_to_string(out), "abcdef");
    EXPECT_EQ(queue.next_read_offset(), 6u);
    EXPECT_EQ(queue.active_extent_count(), 0u);
    EXPECT_EQ(queue.active_block_count(), 0u);
    EXPECT_GE(pool.cached_count(), 1u);
}

TEST(QuicStreamRecvQueueTest, OverlappingInsertCopiesOnlyMissingPrefix) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicStreamRecvQueue queue(pool);

    auto first = fiber::quic::QuicStreamRecvQueueTestAccess::recv_stream_data(queue, 2, slice_of("cdef"));
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(*first, 4u);

    auto second = fiber::quic::QuicStreamRecvQueueTestAccess::recv_stream_data(queue, 0, slice_of("abcd"));
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*second, 2u);
    EXPECT_EQ(queue.active_extent_count(), 1u);
    EXPECT_EQ(queue.buffered_bytes(), 6u);

    fiber::mem::IoBufChain out(pool);
    auto taken = queue.try_take(16, out);
    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(*taken, 6u);
    EXPECT_EQ(chain_to_string(out), "abcdef");
}

TEST(QuicStreamRecvQueueTest, DeliveredDuplicateIsIgnored) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicStreamRecvQueue queue(pool);

    ASSERT_TRUE(fiber::quic::QuicStreamRecvQueueTestAccess::recv_stream_data(queue, 0, slice_of("abc")).has_value());

    fiber::mem::IoBufChain out(pool);
    auto taken = queue.try_take(3, out);
    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(*taken, 3u);
    EXPECT_EQ(chain_to_string(out), "abc");

    auto duplicate = fiber::quic::QuicStreamRecvQueueTestAccess::recv_stream_data(queue, 0, slice_of("abc"));
    ASSERT_TRUE(duplicate.has_value());
    EXPECT_EQ(*duplicate, 0u);
    EXPECT_EQ(queue.received_end_offset(), 3u);

    auto next = fiber::quic::QuicStreamRecvQueueTestAccess::recv_stream_data(queue, 3, slice_of("de"));
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(*next, 2u);
    EXPECT_EQ(queue.received_end_offset(), 5u);

    fiber::mem::IoBufChain tail(pool);
    taken = queue.try_take(8, tail);
    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(*taken, 2u);
    EXPECT_EQ(chain_to_string(tail), "de");
}

TEST(QuicStreamRecvQueueTest, ReceivedEndOffsetTracksMaxAcceptedFrameEnd) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicStreamRecvQueue queue(pool);

    auto late = fiber::quic::QuicStreamRecvQueueTestAccess::recv_stream_data(queue, 10, slice_of("xy"));
    ASSERT_TRUE(late.has_value());
    EXPECT_EQ(*late, 2u);
    EXPECT_EQ(queue.received_end_offset(), 12u);

    auto earlier = fiber::quic::QuicStreamRecvQueueTestAccess::recv_stream_data(queue, 0, slice_of("abc"));
    ASSERT_TRUE(earlier.has_value());
    EXPECT_EQ(*earlier, 3u);
    EXPECT_EQ(queue.received_end_offset(), 12u);

    fiber::quic::QuicSlice empty{};
    auto fin = fiber::quic::QuicStreamRecvQueueTestAccess::recv_stream_data(queue, 12, empty, true);
    ASSERT_TRUE(fin.has_value());
    EXPECT_EQ(*fin, 0u);
    EXPECT_EQ(queue.received_end_offset(), 12u);
    EXPECT_TRUE(queue.has_final_size());
    EXPECT_TRUE(queue.fin_received());
    EXPECT_EQ(queue.final_size(), 12u);

    auto invalid = fiber::quic::QuicStreamRecvQueueTestAccess::recv_stream_data(queue, 12, slice_of("overflow"));
    EXPECT_FALSE(invalid.has_value());
    EXPECT_EQ(queue.received_end_offset(), 12u);
}

TEST(QuicStreamRecvQueueTest, FinalSizeMustRemainConsistent) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicStreamRecvQueue queue(pool);

    auto fin = fiber::quic::QuicStreamRecvQueueTestAccess::recv_stream_data(queue, 0, slice_of("abc"), true);
    ASSERT_TRUE(fin.has_value());
    EXPECT_TRUE(queue.has_final_size());
    EXPECT_TRUE(queue.fin_received());
    EXPECT_EQ(queue.final_size(), 3u);

    auto mismatched_fin =
            fiber::quic::QuicStreamRecvQueueTestAccess::recv_stream_data(queue, 0, slice_of("abcd"), true);
    EXPECT_FALSE(mismatched_fin.has_value());

    fiber::mem::IoBufChain out(pool);
    auto taken = queue.try_take(16, out);
    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(*taken, 3u);
    EXPECT_EQ(chain_to_string(out), "abc");
    EXPECT_TRUE(out.complete());
}

TEST(QuicStreamRecvQueueTest, FirstFrameCanCrossRecvBlockBoundary) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicStreamRecvQueue queue(pool, {
                                                         .buffer_limit = 128 * 1024,
                                                         .low_water = 16 * 1024,
                                                         .max_stream_data = 128 * 1024,
                                                 });

    constexpr std::uint64_t half_k = 512;
    constexpr std::uint64_t start = kStreamRecvBlockSize - half_k;
    std::string payload(kStreamRecvBlockSize / 64, 'x');
    payload.replace(0, 4, "head");
    payload.replace(payload.size() - 4, 4, "tail");

    auto inserted = fiber::quic::QuicStreamRecvQueueTestAccess::recv_stream_data(queue, start, slice_of(payload));
    ASSERT_TRUE(inserted.has_value());
    EXPECT_EQ(*inserted, payload.size());
    EXPECT_EQ(queue.active_extent_count(), 2u);
    EXPECT_EQ(queue.active_block_count(), 2u);

    fiber::mem::IoBufChain empty(pool);
    auto blocked = queue.try_take(payload.size(), empty);
    ASSERT_FALSE(blocked.has_value());
    EXPECT_EQ(blocked.error(), fiber::common::IoErr::WouldBlock);

    std::string prefix(start, 'p');
    inserted = fiber::quic::QuicStreamRecvQueueTestAccess::recv_stream_data(queue, 0, slice_of(prefix));
    ASSERT_TRUE(inserted.has_value());
    EXPECT_EQ(*inserted, prefix.size());
    EXPECT_EQ(queue.active_extent_count(), 2u);
    EXPECT_EQ(queue.active_block_count(), 2u);

    fiber::mem::IoBufChain out(pool);
    auto taken = queue.try_take(prefix.size() + payload.size(), out);
    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(*taken, prefix.size() + payload.size());
    std::string expected = prefix + payload;
    EXPECT_EQ(chain_to_string(out), expected);
    EXPECT_EQ(queue.next_read_offset(), prefix.size() + payload.size());
    EXPECT_EQ(queue.active_extent_count(), 0u);
    EXPECT_EQ(queue.active_block_count(), 0u);
}

TEST(QuicStreamRecvQueueTest, FirstFrameCanStartInSecondRecvBlock) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicStreamRecvQueue queue(pool, {
                                                         .buffer_limit = 128 * 1024,
                                                         .low_water = 16 * 1024,
                                                         .max_stream_data = 128 * 1024,
                                                 });

    constexpr std::uint64_t first_start = kStreamRecvBlockSize + 1024;
    auto inserted = fiber::quic::QuicStreamRecvQueueTestAccess::recv_stream_data(queue, first_start, slice_of("late"));
    ASSERT_TRUE(inserted.has_value());
    EXPECT_EQ(*inserted, 4u);
    EXPECT_EQ(queue.active_extent_count(), 1u);
    EXPECT_EQ(queue.active_block_count(), 1u);

    fiber::mem::IoBufChain out(pool);
    auto blocked = queue.try_take(16, out);
    ASSERT_FALSE(blocked.has_value());
    EXPECT_EQ(blocked.error(), fiber::common::IoErr::WouldBlock);

    std::string block_prefix(1024, 'b');
    inserted = fiber::quic::QuicStreamRecvQueueTestAccess::recv_stream_data(queue, kStreamRecvBlockSize,
                                                                            slice_of(block_prefix));
    ASSERT_TRUE(inserted.has_value());
    EXPECT_EQ(*inserted, block_prefix.size());
    EXPECT_EQ(queue.active_extent_count(), 1u);
    EXPECT_EQ(queue.active_block_count(), 1u);

    std::string first_block(kStreamRecvBlockSize, 'a');
    inserted = fiber::quic::QuicStreamRecvQueueTestAccess::recv_stream_data(queue, 0, slice_of(first_block));
    ASSERT_TRUE(inserted.has_value());
    EXPECT_EQ(*inserted, first_block.size());
    EXPECT_EQ(queue.active_extent_count(), 2u);
    EXPECT_EQ(queue.active_block_count(), 2u);

    auto taken = queue.try_take(first_block.size() + block_prefix.size() + 4, out);
    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(*taken, first_block.size() + block_prefix.size() + 4);
    EXPECT_EQ(chain_to_string(out), first_block + block_prefix + "late");
    EXPECT_EQ(queue.active_extent_count(), 0u);
    EXPECT_EQ(queue.active_block_count(), 0u);
}
