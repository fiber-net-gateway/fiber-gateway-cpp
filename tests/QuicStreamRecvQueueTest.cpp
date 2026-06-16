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

    fiber::mem::IoBufChain out(pool);
    auto taken = queue.try_take(payload.size(), out);
    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(*taken, payload.size());

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
