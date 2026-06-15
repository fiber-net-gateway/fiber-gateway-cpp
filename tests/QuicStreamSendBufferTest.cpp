#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <string>
#include <string_view>

#include "quic/QuicStreamSendBuffer.h"
#include "quic/QuicTransportCodec.h"

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
    fiber::quic::QuicStreamDataExtentPool pool;
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
    fiber::quic::QuicStreamDataExtentPool pool;
    fiber::quic::QuicStreamSendBuffer buffer(pool);

    ASSERT_TRUE(buffer.append(iobuf_of("abcdef")).has_value());

    std::array<std::uint8_t, 6> out{};
    auto encoded = buffer.encode_stream_frame(4, out.data(), out.size());
    ASSERT_TRUE(encoded.has_value());
    ASSERT_TRUE(encoded->encoded);
    EXPECT_EQ(encoded->data_len, 3u);
    EXPECT_EQ(buffer.ready_bytes(), 3u);
    EXPECT_EQ(buffer.inflight_bytes(), 3u);

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
    fiber::quic::QuicStreamDataExtentPool pool;
    fiber::quic::QuicStreamSendBuffer buffer(pool);

    ASSERT_TRUE(buffer.append(iobuf_of("abc")).has_value());

    std::array<std::uint8_t, 3> out{};
    auto encoded = buffer.encode_stream_frame(4, out.data(), out.size());
    ASSERT_TRUE(encoded.has_value());
    EXPECT_FALSE(encoded->encoded);
    EXPECT_EQ(buffer.ready_bytes(), 3u);
    EXPECT_EQ(buffer.inflight_bytes(), 0u);
}

TEST(QuicStreamSendBufferTest, LargeIoBufAppendsAsSingleExtent) {
    fiber::quic::QuicStreamDataExtentPool pool;
    fiber::quic::QuicStreamSendBuffer buffer(pool);

    std::string payload(fiber::quic::kQuicStreamDataBlockSize + 17, 'x');
    auto appended = buffer.append(iobuf_of(payload));
    ASSERT_TRUE(appended.has_value());
    EXPECT_EQ(*appended, payload.size());
    EXPECT_EQ(buffer.active_extent_count(), 1u);
    EXPECT_EQ(buffer.ready_bytes(), payload.size());
}

TEST(QuicStreamSendBufferTest, FailedPartialEncodeMergesReadySlicesFromSameIoBuf) {
    fiber::quic::QuicStreamDataExtentPool pool;
    fiber::quic::QuicStreamSendBuffer buffer(pool);

    ASSERT_TRUE(buffer.append(iobuf_of("abcdef")).has_value());

    std::array<std::uint8_t, 6> out{};
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
    fiber::quic::QuicStreamDataExtentPool pool;
    fiber::quic::QuicStreamSendBuffer buffer(pool);

    ASSERT_TRUE(buffer.append(iobuf_of("abc")).has_value());
    ASSERT_TRUE(buffer.append(iobuf_of("def")).has_value());

    EXPECT_EQ(buffer.buffered_bytes(), 6u);
    EXPECT_EQ(buffer.ready_bytes(), 6u);
    EXPECT_EQ(buffer.active_extent_count(), 2u);
}

TEST(QuicStreamSendBufferTest, FinalDataCarriesFinUntilAcked) {
    fiber::quic::QuicStreamDataExtentPool pool;
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
    fiber::quic::QuicStreamDataExtentPool pool;
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

} // namespace
