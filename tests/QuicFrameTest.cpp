#include <gtest/gtest.h>

#include <array>
#include <utility>

#include <fiber/quic/QuicFrame.h>
#include "quic/QuicTransportCodec.h"

namespace {

using namespace fiber::quic;

class QuicOutputFrameDataTest : public testing::TestWithParam<QuicFrameType> {};

TEST_P(QuicOutputFrameDataTest, EncodesSharedSliceAfterOriginalFrameIsDestroyed) {
    QuicOutputFrame copy{};
    {
        QuicOutputFrame original{};
        original.type = GetParam();
        if (original.type == QuicFrameType::Ack || original.type == QuicFrameType::AckEcn) {
            original.u.ack.largest = 10;
            original.u.ack.range_count = 1;
        }
        std::array<std::uint8_t, 4> bytes{0xff, 1, 2, 0xff};
        ASSERT_TRUE(quic_output_frame_set_owned_data(original, bytes.data(), bytes.size()).has_value());
        bytes.fill(0);
        original.data = original.data.retain_slice(1, 2);
        copy = original;
        EXPECT_TRUE(copy.data.same_storage(original.data));
    }
    EXPECT_TRUE(copy.data.unique());
    QuicOutputFrame moved = std::move(copy);
    EXPECT_FALSE(copy.data);
    ASSERT_EQ(moved.data.readable(), 2U);

    std::array<std::uint8_t, 64> bytes{};
    QuicWriteCursor out(bytes.data(), bytes.size());
    auto encoded_len = quic_output_frame_encoded_len(moved);
    ASSERT_TRUE(encoded_len.has_value());
    auto written = quic_create_output_frame(&out, moved);
    ASSERT_TRUE(written.has_value());
    EXPECT_EQ(*written, *encoded_len);

    QuicReadCursor in(bytes.data(), out.offset());
    auto parsed = quic_parse_frame_for_receiver(QuicConnectionRole::Client, QuicEncryptionLevel::Application, in);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->frame.type, GetParam());
    EXPECT_TRUE(in.empty());
    const QuicSlice payload =
            GetParam() == QuicFrameType::ConnectionClose || GetParam() == QuicFrameType::ConnectionCloseApp
                    ? parsed->frame.u.close.reason
                    : parsed->frame.data;
    ASSERT_EQ(payload.len, 2U);
    EXPECT_EQ(payload.data[0], 1U);
    EXPECT_EQ(payload.data[1], 2U);
}

TEST_P(QuicOutputFrameDataTest, PoolReleasesPayloadBeforeReusingFrame) {
    QuicOutputFramePool pool;
    QuicOutputFrame *frame = pool.alloc();
    ASSERT_NE(frame, nullptr);
    frame->type = GetParam();
    constexpr std::array<std::uint8_t, 2> bytes{1, 2};
    ASSERT_TRUE(quic_output_frame_set_owned_data(*frame, bytes.data(), bytes.size()).has_value());
    fiber::mem::IoBuf retained = frame->data;
    pool.release(frame);
    EXPECT_TRUE(retained.unique());
    ASSERT_EQ(pool.cached_count(), 1U);

    QuicOutputFrame *reused = pool.alloc();
    ASSERT_EQ(reused, frame);
    EXPECT_FALSE(reused->data);
    EXPECT_EQ(reused->type, QuicFrameType::Padding);
    pool.release(reused);
}

INSTANTIATE_TEST_SUITE_P(PayloadTypes, QuicOutputFrameDataTest,
                         testing::Values(QuicFrameType::Ack, QuicFrameType::AckEcn, QuicFrameType::Crypto,
                                         QuicFrameType::NewToken, QuicFrameType::ConnectionClose,
                                         QuicFrameType::ConnectionCloseApp));

TEST(QuicOutputFrameTest, RejectsInvalidPayloadWithoutChangingFrame) {
    QuicOutputFrame frame{};
    constexpr std::uint8_t byte = 7;
    const auto invalid = std::unexpected(fiber::common::IoErr::Invalid);
    EXPECT_EQ(quic_output_frame_set_owned_data(frame, &byte, 1), invalid);
    frame.type = QuicFrameType::NewToken;
    EXPECT_EQ(quic_output_frame_set_owned_data(frame, nullptr, 1), invalid);
    EXPECT_EQ(quic_output_frame_set_owned_data(frame, &byte, std::size_t{UINT32_MAX} + 1), invalid);
    EXPECT_FALSE(frame.data);

    ASSERT_TRUE(quic_output_frame_set_owned_data(frame, &byte, 1).has_value());
    auto encoded_len = quic_output_frame_encoded_len(frame);
    ASSERT_TRUE(encoded_len.has_value());
    const auto *data = frame.data.readable_data();
    EXPECT_EQ(quic_output_frame_set_owned_data(frame, &byte, 1), invalid);
    EXPECT_EQ(quic_output_frame_set_owned_data(frame, nullptr, 0), invalid);
    EXPECT_EQ(frame.data.readable_data(), data);
    EXPECT_EQ(frame.data.readable(), 1U);
    EXPECT_EQ(frame.encoded_len, *encoded_len);
}

TEST(QuicOutputFrameTest, SettingPayloadInvalidatesEncodedLength) {
    QuicOutputFrame frame{};
    frame.type = QuicFrameType::ConnectionClose;
    ASSERT_TRUE(quic_output_frame_set_owned_data(frame, nullptr, 0).has_value());
    EXPECT_FALSE(frame.data);
    auto empty_len = quic_output_frame_encoded_len(frame);
    ASSERT_TRUE(empty_len.has_value());

    constexpr std::array<std::uint8_t, 3> reason{'b', 'a', 'd'};
    ASSERT_TRUE(quic_output_frame_set_owned_data(frame, reason.data(), reason.size()).has_value());
    EXPECT_EQ(frame.encoded_len, 0U);
    auto filled_len = quic_output_frame_encoded_len(frame);
    ASSERT_TRUE(filled_len.has_value());
    EXPECT_EQ(*filled_len, *empty_len + reason.size());
}

} // namespace
