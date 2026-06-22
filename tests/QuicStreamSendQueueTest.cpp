#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <string_view>

#include "quic/QuicStreamSendQueue.h"

namespace fiber::quic {

struct QuicStreamSendQueueTestAccess {
    static common::IoResult<QuicStreamSendQueue::EncodedFrameResult>
    encode_stream_frame(QuicStreamSendQueue &queue, std::uint64_t stream_id, std::uint8_t *dst,
                        std::size_t capacity) noexcept {
        return queue.encode_stream_frame(stream_id, dst, capacity);
    }

    static common::IoResult<void> mark_acked(QuicStreamSendQueue &queue, std::size_t offset, std::size_t length,
                                             bool encoded_fin) noexcept {
        return queue.mark_acked(offset, length, encoded_fin);
    }
};

} // namespace fiber::quic

namespace {

fiber::mem::IoBuf iobuf_of(std::string_view value) {
    fiber::mem::IoBuf buf = fiber::mem::IoBuf::allocate(value.size());
    if (!buf) {
        return {};
    }
    if (!value.empty()) {
        std::memcpy(buf.writable_data(), value.data(), value.size());
        buf.commit(value.size());
    }
    return buf;
}

} // namespace

TEST(QuicStreamSendQueueTest, AppendReturnsWouldBlockWhenBufferFull) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicStreamSendQueue queue(pool, {.buffer_limit = 5});

    ASSERT_TRUE(queue.try_append(iobuf_of("hello")).has_value());
    EXPECT_EQ(queue.buffer_available(), 0u);

    auto result = queue.try_append(iobuf_of("!"));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), fiber::common::IoErr::WouldBlock);
}

TEST(QuicStreamSendQueueTest, AckReleasesBufferedBytes) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicStreamSendQueue queue(pool, {.buffer_limit = 5});

    ASSERT_TRUE(queue.try_append(iobuf_of("hello")).has_value());

    std::array<std::uint8_t, 64> out{};
    auto encoded = fiber::quic::QuicStreamSendQueueTestAccess::encode_stream_frame(queue, 4, out.data(), out.size());
    ASSERT_TRUE(encoded.has_value());
    ASSERT_TRUE(encoded->encoded);
    ASSERT_EQ(encoded->data_len, 5u);

    auto acked = fiber::quic::QuicStreamSendQueueTestAccess::mark_acked(queue, encoded->offset, encoded->data_len,
                                                                        encoded->fin);
    ASSERT_TRUE(acked.has_value());

    EXPECT_EQ(queue.buffer_available(), 5u);
    EXPECT_EQ(queue.inflight_bytes(), 0u);
    EXPECT_EQ(queue.ready_bytes(), 0u);
}

TEST(QuicStreamSendQueueTest, AppendLargerThanBufferLimitFailsImmediately) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicStreamSendQueue queue(pool, {.buffer_limit = 2});

    auto result = queue.try_append(iobuf_of("abc"));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), fiber::common::IoErr::MessageTooLarge);
}

TEST(QuicStreamSendQueueTest, ResetClearsBufferedDataAndRejectsFurtherAppend) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicStreamSendQueue queue(pool, {.buffer_limit = 5});

    ASSERT_TRUE(queue.try_append(iobuf_of("hello")).has_value());

    auto reset = queue.reset(42);
    ASSERT_TRUE(reset.has_value());
    EXPECT_EQ(*reset, 5u);
    EXPECT_TRUE(queue.reset_sent());
    EXPECT_EQ(queue.final_size(), 5u);
    EXPECT_EQ(queue.reset_error_code(), 42u);
    EXPECT_EQ(queue.buffer_available(), 5u);

    auto append_after_reset = queue.try_append(iobuf_of("x"));
    ASSERT_FALSE(append_after_reset.has_value());
    EXPECT_EQ(append_after_reset.error(), fiber::common::IoErr::BrokenPipe);
}
