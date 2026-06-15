#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <string>
#include <string_view>

#include "quic/QuicStreamSendBuffer.h"
#include "quic/QuicTransportCodec.h"

namespace {

constexpr std::size_t kStreamDataBlockSize = 64 * 1024;

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

fiber::quic::QuicInputFrame parse_stream_frame(const std::uint8_t *data, std::size_t len) {
    fiber::quic::QuicReadCursor cursor(data, len);
    auto parsed = fiber::quic::quic_parse_frame(fiber::quic::QuicEncryptionLevel::Application, cursor);
    EXPECT_TRUE(parsed.has_value());
    EXPECT_EQ(cursor.remaining(), 0u);
    return parsed->frame;
}

std::string_view frame_data_view(const fiber::quic::QuicInputFrame &frame) {
    return {reinterpret_cast<const char *>(frame.data.data), frame.data.len};
}

TEST(QuicStreamSendBufferTest, EncodesAppendedDataAsInflightAndAckReleasesIt) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicStreamSendBuffer buffer(pool);

    auto appended = buffer.append(iobuf_of("hello"));
    ASSERT_TRUE(appended.has_value());
    EXPECT_EQ(*appended, 5u);
    EXPECT_EQ(buffer.ready_bytes(), 5u);

    std::array<std::uint8_t, 64> out{};
    auto encoded = buffer.encode_stream_frame(4, out.data(), out.size());
    ASSERT_TRUE(encoded.has_value());
    ASSERT_TRUE(encoded->encoded);
    EXPECT_EQ(encoded->offset, 0u);
    EXPECT_EQ(encoded->data_len, 5u);
    EXPECT_FALSE(encoded->fin);
    EXPECT_EQ(buffer.ready_bytes(), 0u);
    EXPECT_EQ(buffer.inflight_bytes(), 5u);

    fiber::quic::QuicInputFrame frame = parse_stream_frame(out.data(), encoded->encoded_len);
    EXPECT_EQ(frame.u.stream.stream_id, 4u);
    EXPECT_EQ(frame.u.stream.offset, 0u);
    EXPECT_EQ(frame.u.stream.length, 5u);
    EXPECT_FALSE(frame.u.stream.fin);
    EXPECT_EQ(frame_data_view(frame), "hello");

    auto acked = buffer.mark_acked(encoded->offset, encoded->data_len, encoded->fin);
    ASSERT_TRUE(acked.has_value());
    EXPECT_TRUE(buffer.empty());
    EXPECT_EQ(buffer.buffered_bytes(), 0u);
    EXPECT_EQ(buffer.active_extent_count(), 0u);
}

TEST(QuicStreamSendBufferTest, FailedRangeReturnsToReadyAndCanBeEncodedAgain) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicStreamSendBuffer buffer(pool);

    ASSERT_TRUE(buffer.append(iobuf_of("abcdef")).has_value());

    std::array<std::uint8_t, 6> out{};
    auto encoded = buffer.encode_stream_frame(4, out.data(), out.size());
    ASSERT_TRUE(encoded.has_value());
    ASSERT_TRUE(encoded->encoded);
    EXPECT_EQ(encoded->data_len, 4u);
    EXPECT_EQ(buffer.ready_bytes(), 2u);
    EXPECT_EQ(buffer.inflight_bytes(), 4u);

    auto failed = buffer.mark_failed(encoded->offset, encoded->data_len, encoded->fin);
    ASSERT_TRUE(failed.has_value());
    EXPECT_EQ(buffer.ready_bytes(), 6u);
    EXPECT_EQ(buffer.inflight_bytes(), 0u);
    EXPECT_EQ(buffer.active_extent_count(), 1u);

    std::array<std::uint8_t, 64> retry{};
    auto retried = buffer.encode_stream_frame(4, retry.data(), retry.size());
    ASSERT_TRUE(retried.has_value());
    ASSERT_TRUE(retried->encoded);
    EXPECT_EQ(retried->offset, 0u);
    EXPECT_EQ(retried->data_len, 6u);

    fiber::quic::QuicInputFrame frame = parse_stream_frame(retry.data(), retried->encoded_len);
    EXPECT_EQ(frame_data_view(frame), "abcdef");
}

TEST(QuicStreamSendBufferTest, InsufficientCapacityDoesNotChangeState) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicStreamSendBuffer buffer(pool);

    ASSERT_TRUE(buffer.append(iobuf_of("abc")).has_value());

    std::array<std::uint8_t, 2> out{};
    auto encoded = buffer.encode_stream_frame(4, out.data(), out.size());
    ASSERT_TRUE(encoded.has_value());
    EXPECT_FALSE(encoded->encoded);
    EXPECT_EQ(buffer.ready_bytes(), 3u);
    EXPECT_EQ(buffer.inflight_bytes(), 0u);
}

TEST(QuicStreamSendBufferTest, LargeIoBufAppendsAsSingleExtent) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicStreamSendBuffer buffer(pool);

    std::string payload(kStreamDataBlockSize + 17, 'x');
    auto appended = buffer.append(iobuf_of(payload));
    ASSERT_TRUE(appended.has_value());
    EXPECT_EQ(*appended, payload.size());
    EXPECT_EQ(buffer.active_extent_count(), 1u);
    EXPECT_EQ(buffer.ready_bytes(), payload.size());
}

TEST(QuicStreamSendBufferTest, FailedPartialEncodeMergesReadySlicesFromSameIoBuf) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicStreamSendBuffer buffer(pool);

    ASSERT_TRUE(buffer.append(iobuf_of("abcdef")).has_value());

    std::array<std::uint8_t, 5> out{};
    auto encoded = buffer.encode_stream_frame(4, out.data(), out.size());
    ASSERT_TRUE(encoded.has_value());
    ASSERT_TRUE(encoded->encoded);
    EXPECT_EQ(encoded->data_len, 3u);
    EXPECT_EQ(buffer.active_extent_count(), 2u);

    auto failed = buffer.mark_failed(encoded->offset, encoded->data_len, encoded->fin);
    ASSERT_TRUE(failed.has_value());
    EXPECT_EQ(buffer.active_extent_count(), 1u);
    EXPECT_EQ(buffer.ready_bytes(), 6u);
}

TEST(QuicStreamSendBufferTest, SeparateIoBufAppendsRemainSeparateReadyExtents) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicStreamSendBuffer buffer(pool);

    ASSERT_TRUE(buffer.append(iobuf_of("abc")).has_value());
    ASSERT_TRUE(buffer.append(iobuf_of("def")).has_value());

    EXPECT_EQ(buffer.buffered_bytes(), 6u);
    EXPECT_EQ(buffer.ready_bytes(), 6u);
    EXPECT_EQ(buffer.active_extent_count(), 2u);
}

TEST(QuicStreamSendBufferTest, FinalDataCarriesFinUntilAcked) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicStreamSendBuffer buffer(pool);

    auto appended = buffer.append(iobuf_of("done"), true);
    ASSERT_TRUE(appended.has_value());
    EXPECT_TRUE(buffer.has_final_size());
    EXPECT_EQ(buffer.final_size(), 4u);

    std::array<std::uint8_t, 64> out{};
    auto encoded = buffer.encode_stream_frame(8, out.data(), out.size());
    ASSERT_TRUE(encoded.has_value());
    ASSERT_TRUE(encoded->encoded);
    EXPECT_EQ(encoded->offset, 0u);
    EXPECT_EQ(encoded->data_len, 4u);
    EXPECT_TRUE(encoded->fin);

    fiber::quic::QuicInputFrame frame = parse_stream_frame(out.data(), encoded->encoded_len);
    EXPECT_TRUE(frame.u.stream.fin);
    EXPECT_EQ(frame_data_view(frame), "done");

    auto acked = buffer.mark_acked(encoded->offset, encoded->data_len, encoded->fin);
    ASSERT_TRUE(acked.has_value());
    EXPECT_TRUE(buffer.has_final_size());
    EXPECT_EQ(buffer.final_size(), 4u);
    EXPECT_TRUE(buffer.empty());
}

TEST(QuicStreamSendBufferTest, FinOnlyFrameCanFailAndRetry) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicStreamSendBuffer buffer(pool);

    ASSERT_TRUE(buffer.append(fiber::mem::IoBuf{}, true).has_value());

    std::array<std::uint8_t, 64> out{};
    auto encoded = buffer.encode_stream_frame(12, out.data(), out.size());
    ASSERT_TRUE(encoded.has_value());
    ASSERT_TRUE(encoded->encoded);
    EXPECT_EQ(encoded->offset, 0u);
    EXPECT_EQ(encoded->data_len, 0u);
    EXPECT_TRUE(encoded->fin);
    EXPECT_FALSE(buffer.empty());

    fiber::quic::QuicInputFrame frame = parse_stream_frame(out.data(), encoded->encoded_len);
    EXPECT_TRUE(frame.u.stream.fin);
    EXPECT_EQ(frame.u.stream.length, 0u);

    ASSERT_TRUE(buffer.mark_failed(encoded->offset, encoded->data_len, encoded->fin).has_value());
    auto retried = buffer.encode_stream_frame(12, out.data(), out.size());
    ASSERT_TRUE(retried.has_value());
    ASSERT_TRUE(retried->encoded);
    EXPECT_TRUE(retried->fin);

    ASSERT_TRUE(buffer.mark_acked(retried->offset, retried->data_len, retried->fin).has_value());
    EXPECT_TRUE(buffer.has_final_size());
    EXPECT_EQ(buffer.final_size(), 0u);
    EXPECT_TRUE(buffer.empty());
}

TEST(QuicStreamSendBufferTest, OmitsLengthFieldWhenPayloadFillsBuffer) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicStreamSendBuffer buffer(pool);

    // 20 bytes of data — more than any reasonable header, so the buffer-filling path triggers.
    ASSERT_TRUE(buffer.append(iobuf_of("01234567890123456789")).has_value());

    // capacity = 22: header (type=1 + stream_id=1) = 2, so remaining = 20 = available → no LEN.
    constexpr std::size_t capacity = 22;
    std::array<std::uint8_t, capacity> out{};
    auto encoded = buffer.encode_stream_frame(4, out.data(), out.size());
    ASSERT_TRUE(encoded.has_value());
    ASSERT_TRUE(encoded->encoded);
    EXPECT_EQ(encoded->data_len, 20u);
    EXPECT_EQ(encoded->encoded_len, capacity); // fills buffer exactly

    // Verify LEN bit (0x02) is NOT set in the type byte.
    EXPECT_EQ(out[0] & 0x02, 0u);

    // Parser must reconstruct the frame correctly using remaining bytes as length.
    fiber::quic::QuicInputFrame frame = parse_stream_frame(out.data(), encoded->encoded_len);
    EXPECT_EQ(frame.u.stream.stream_id, 4u);
    EXPECT_EQ(frame.u.stream.offset, 0u);
    EXPECT_EQ(frame.u.stream.length, 20u);
    EXPECT_FALSE(frame.u.stream.fin);
    EXPECT_FALSE(frame.u.stream.has_length);
    EXPECT_EQ(frame_data_view(frame), "01234567890123456789");
}

TEST(QuicStreamSendBufferTest, IncludesLengthFieldWhenPayloadDoesNotFillBuffer) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicStreamSendBuffer buffer(pool);

    ASSERT_TRUE(buffer.append(iobuf_of("hi")).has_value());

    // capacity = 64 but only 2 bytes of data → cannot fill → LEN must be set.
    std::array<std::uint8_t, 64> out{};
    auto encoded = buffer.encode_stream_frame(4, out.data(), out.size());
    ASSERT_TRUE(encoded.has_value());
    ASSERT_TRUE(encoded->encoded);
    EXPECT_EQ(encoded->data_len, 2u);

    // Verify LEN bit (0x02) IS set in the type byte.
    EXPECT_EQ(out[0] & 0x02, 0x02u);

    fiber::quic::QuicInputFrame frame = parse_stream_frame(out.data(), encoded->encoded_len);
    EXPECT_TRUE(frame.u.stream.has_length);
    EXPECT_EQ(frame.u.stream.length, 2u);
    EXPECT_EQ(frame_data_view(frame), "hi");
}

TEST(QuicStreamSendBufferTest, OmitsLengthWithOffsetAndFin) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicStreamSendBuffer buffer(pool);

    // Append first 5 bytes without FIN, then 5 more bytes with FIN.
    ASSERT_TRUE(buffer.append(iobuf_of("abcde")).has_value());
    ASSERT_TRUE(buffer.append(iobuf_of("fghij"), true).has_value());

    // First frame: encode 5 bytes at offset 0 with large capacity → uses LEN.
    std::array<std::uint8_t, 64> out1{};
    auto enc1 = buffer.encode_stream_frame(4, out1.data(), out1.size());
    ASSERT_TRUE(enc1.has_value());
    ASSERT_TRUE(enc1->encoded);
    EXPECT_EQ(enc1->offset, 0u);
    EXPECT_EQ(enc1->data_len, 5u);
    EXPECT_FALSE(enc1->fin);
    ASSERT_TRUE(buffer.mark_acked(enc1->offset, enc1->data_len, enc1->fin).has_value());

    // Second frame: 5 bytes at offset 5 with FIN.
    // capacity=8: header(type=1 + sid=1 + off=1)=3, remaining=5=available → no LEN.
    std::array<std::uint8_t, 8> out2{};
    auto enc2 = buffer.encode_stream_frame(4, out2.data(), out2.size());
    ASSERT_TRUE(enc2.has_value());
    ASSERT_TRUE(enc2->encoded);
    EXPECT_EQ(enc2->offset, 5u);
    EXPECT_EQ(enc2->data_len, 5u);
    EXPECT_TRUE(enc2->fin);
    EXPECT_EQ(enc2->encoded_len, 8u); // fills buffer exactly

    // LEN bit must not be set, but OFF and FIN bits must be set.
    EXPECT_EQ(out2[0] & 0x02, 0u); // no LEN
    EXPECT_NE(out2[0] & 0x04, 0u); // has OFF
    EXPECT_NE(out2[0] & 0x01, 0u); // has FIN

    fiber::quic::QuicInputFrame frame = parse_stream_frame(out2.data(), enc2->encoded_len);
    EXPECT_EQ(frame.u.stream.offset, 5u);
    EXPECT_EQ(frame.u.stream.length, 5u);
    EXPECT_TRUE(frame.u.stream.fin);
    EXPECT_FALSE(frame.u.stream.has_length);
    EXPECT_EQ(frame_data_view(frame), "fghij");
}

} // namespace
