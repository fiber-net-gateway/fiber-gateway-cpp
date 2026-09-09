#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <string_view>

#include <fiber/quic/QuicStreamSendQueue.h>

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

    static common::IoResult<void> mark_failed(QuicStreamSendQueue &queue, std::size_t offset, std::size_t length,
                                              bool encoded_fin) noexcept {
        return queue.mark_failed(offset, length, encoded_fin);
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

TEST(QuicStreamSendQueueTest, FinOnlyFrameEncodesWhileBodyInflight) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicStreamSendQueue queue(pool);

    // Body extent encoded into a packet: no ready bytes remain, body still inflight.
    ASSERT_TRUE(queue.try_append(iobuf_of("hello")).has_value());
    std::array<std::uint8_t, 64> out{};
    auto body = fiber::quic::QuicStreamSendQueueTestAccess::encode_stream_frame(queue, 4, out.data(), out.size());
    ASSERT_TRUE(body.has_value());
    ASSERT_TRUE(body->encoded);
    ASSERT_FALSE(body->fin);
    ASSERT_EQ(queue.ready_bytes(), 0u);
    ASSERT_EQ(queue.inflight_bytes(), 5u);

    // Terminal write: zero-length append carrying the FIN (final_size = 5).
    fiber::mem::IoBuf fin_chunk;
    ASSERT_TRUE(queue.try_append(fin_chunk, true).has_value());
    ASSERT_EQ(queue.final_size(), 5u);

    // The FIN-only frame must be encodable while the body is still inflight:
    // offsets are absolute and the peer reassembles out of order. The old
    // buffered_bytes() > 0 guard refused this and stranded the FIN until some
    // unrelated send activity bailed it out.
    auto fin = fiber::quic::QuicStreamSendQueueTestAccess::encode_stream_frame(queue, 4, out.data(), out.size());
    ASSERT_TRUE(fin.has_value());
    EXPECT_TRUE(fin->encoded);
    EXPECT_TRUE(fin->fin);
    EXPECT_EQ(fin->offset, 5u);
    EXPECT_EQ(fin->data_len, 0u);
    EXPECT_GT(fin->encoded_len, 0u);

    // Loss of the FIN-only frame: mark_failed resets fin_inflight_ so it re-encodes.
    ASSERT_TRUE(fiber::quic::QuicStreamSendQueueTestAccess::mark_failed(queue, fin->offset, fin->data_len, fin->fin)
                        .has_value());
    auto retransmit = fiber::quic::QuicStreamSendQueueTestAccess::encode_stream_frame(queue, 4, out.data(), out.size());
    ASSERT_TRUE(retransmit.has_value());
    EXPECT_TRUE(retransmit->encoded);
    EXPECT_TRUE(retransmit->fin);
    EXPECT_EQ(retransmit->offset, 5u);

    // ACK of body and FIN clears the queue.
    ASSERT_TRUE(fiber::quic::QuicStreamSendQueueTestAccess::mark_acked(queue, body->offset, body->data_len, body->fin)
                        .has_value());
    ASSERT_TRUE(fiber::quic::QuicStreamSendQueueTestAccess::mark_acked(queue, retransmit->offset, retransmit->data_len,
                                                                       retransmit->fin)
                        .has_value());
    EXPECT_EQ(queue.buffered_bytes(), 0u);
    EXPECT_TRUE(queue.fin_acked());
    EXPECT_TRUE(queue.empty());
}

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
