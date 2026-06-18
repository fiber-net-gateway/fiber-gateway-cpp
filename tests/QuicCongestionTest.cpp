#include <gtest/gtest.h>

#include <limits>

#include "quic/QuicAckHandler.h"
#include "quic/QuicCongestion.h"
#include "quic/QuicConnection.h"

TEST(QuicCongestionTest, InitializesLikeNginx) {
    fiber::quic::QuicCongestionState cg{};
    fiber::quic::quic_congestion_init(cg, fiber::quic::QuicTime{0});

    EXPECT_EQ(cg.window, 12000U);
    EXPECT_EQ(cg.ssthresh, std::numeric_limits<std::size_t>::max());
    EXPECT_EQ(cg.mtu, fiber::quic::kQuicCongestionMinInitialSize);
    EXPECT_EQ(cg.recovery_start, fiber::quic::QuicTime{-1});
}

TEST(QuicCongestionTest, SlowStartAddsAckedPacketLength) {
    fiber::quic::QuicCongestionState cg{};
    fiber::quic::quic_congestion_init(cg, fiber::quic::QuicTime{0});

    fiber::quic::quic_congestion_on_packet_sent(cg, 1200, true, false);
    const bool unblocked =
            fiber::quic::quic_congestion_on_ack(cg, fiber::quic::QuicAckSample{1200, 0, fiber::quic::QuicTime{0}}, 0,
                                                fiber::quic::QuicTime{50}, fiber::quic::QuicTime{0});

    EXPECT_FALSE(unblocked);
    EXPECT_EQ(cg.in_flight, 0U);
    EXPECT_EQ(cg.window, 13200U);
}

TEST(QuicCongestionTest, LossUsesNginxMultiplicativeDecrease) {
    fiber::quic::QuicCongestionState cg{};
    fiber::quic::quic_congestion_init(cg, fiber::quic::QuicTime{0});
    fiber::quic::quic_congestion_on_packet_sent(cg, 12000, true, false);

    const bool unblocked = fiber::quic::quic_congestion_on_loss(
            cg, fiber::quic::QuicLossSample{1200, 0, fiber::quic::QuicTime{0}, false}, 0, fiber::quic::QuicTime{100},
            1200);

    EXPECT_FALSE(unblocked);
    EXPECT_EQ(cg.in_flight, 10800U);
    EXPECT_EQ(cg.ssthresh, 7560U);
    EXPECT_EQ(cg.window, 7560U);
    EXPECT_EQ(cg.w_max, 12000U);
    EXPECT_EQ(cg.recovery_start, fiber::quic::QuicTime{100});
}

TEST(QuicCongestionTest, RttSampleMatchesQuicEstimatorShape) {
    fiber::quic::QuicRttState rtt{};
    fiber::quic::quic_rtt_init(rtt);

    fiber::quic::quic_rtt_sample(rtt, fiber::quic::QuicTime{100}, fiber::quic::QuicTime{20}, 0, 3,
                                 fiber::quic::QuicTime{25}, false);

    EXPECT_EQ(rtt.latest_rtt, fiber::quic::QuicTime{80});
    EXPECT_EQ(rtt.min_rtt, fiber::quic::QuicTime{80});
    EXPECT_EQ(rtt.avg_rtt, fiber::quic::QuicTime{80});
    EXPECT_EQ(rtt.rttvar, fiber::quic::QuicTime{40});
}

TEST(QuicAckHandlerTest, AckedSentFrameUpdatesCongestionAndRtt) {
    fiber::quic::QuicConnection::Options options{};
    options.role = fiber::quic::QuicConnectionRole::Server;
    fiber::quic::QuicConnection connection(options);
    auto &space = connection.packet_number_space(fiber::quic::QuicEncryptionLevel::Initial);

    fiber::quic::QuicOutputFrame *frame = space.alloc_frame();
    ASSERT_NE(frame, nullptr);
    frame->type = fiber::quic::QuicFrameType::Ping;
    frame->packet_number = 0;
    frame->packet_len = 1200;
    frame->send_time = fiber::quic::QuicTime{10};
    frame->packet_ack_eliciting = true;
    space.next_packet_number = 1;
    space.sent_frames.push_back(*frame);
    fiber::quic::quic_congestion_on_packet_sent(connection.congestion(), 1200, true, false);

    fiber::quic::QuicInputFrame ack{};
    ack.type = fiber::quic::QuicFrameType::Ack;
    ack.level = fiber::quic::QuicEncryptionLevel::Initial;
    ack.u.ack.largest = 0;
    ack.u.ack.delay = 0;
    ack.u.ack.range_count = 0;
    ack.u.ack.first_range = 0;

    auto result = fiber::quic::quic_handle_ack_frame(connection, fiber::quic::QuicEncryptionLevel::Initial, ack,
                                                     fiber::quic::QuicTime{90});

    ASSERT_TRUE(result.has_value()) << static_cast<int>(result.error());
    EXPECT_TRUE(result->acked_frames);
    EXPECT_TRUE(space.sent_frames.empty());
    EXPECT_EQ(connection.congestion().in_flight, 0U);
    EXPECT_EQ(connection.congestion().window, 13200U);
    EXPECT_EQ(connection.rtt().latest_rtt, fiber::quic::QuicTime{80});
}
