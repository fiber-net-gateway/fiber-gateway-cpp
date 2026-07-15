#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>

#include "quic/QuicAckHandler.h"
#include "quic/QuicCongestion.h"
#include "quic/QuicConnection.h"
#include "quic/QuicLossRecovery.h"

#include "QuicTestLoop.h"

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

TEST(QuicOutputFrameQueueTest, MaintainsReverseLinksAcrossMutations) {
    fiber::quic::QuicOutputFrame first{};
    fiber::quic::QuicOutputFrame second{};
    fiber::quic::QuicOutputFrame third{};
    fiber::quic::QuicOutputFrame fourth{};
    fiber::quic::QuicOutputFrame fifth{};
    fiber::quic::QuicOutputFrameQueue queue{};

    queue.push_back(first);
    queue.push_back(second);
    queue.push_front(third);
    queue.insert_after(first, fourth);
    queue.erase_after(&first, fourth);

    fiber::quic::QuicOutputFrameQueue prefix{};
    prefix.push_back(fourth);
    prefix.push_back(fifth);
    queue.prepend_all(prefix);

    const std::array expected{&fourth, &fifth, &third, &first, &second};
    fiber::quic::QuicOutputFrame *previous = nullptr;
    fiber::quic::QuicOutputFrame *frame = queue.front();
    for (fiber::quic::QuicOutputFrame *item: expected) {
        ASSERT_EQ(frame, item);
        EXPECT_EQ(queue.prev_of(*frame), previous);
        previous = frame;
        frame = queue.next_of(*frame);
    }
    EXPECT_EQ(frame, nullptr);
    EXPECT_EQ(queue.back(), expected.back());

    fiber::quic::QuicOutputFrame *next = nullptr;
    frame = queue.back();
    for (auto it = expected.rbegin(); it != expected.rend(); ++it) {
        ASSERT_EQ(frame, *it);
        EXPECT_EQ(queue.next_of(*frame), next);
        next = frame;
        frame = queue.prev_of(*frame);
    }
    EXPECT_EQ(frame, nullptr);

    for (std::size_t i = 0; i < expected.size(); ++i) {
        fiber::quic::QuicOutputFrame *removed = queue.pop_front();
        ASSERT_EQ(removed, expected[i]);
        EXPECT_EQ(removed->next, nullptr);
        EXPECT_EQ(removed->prev, nullptr);
        EXPECT_FALSE(removed->queued);
    }
    EXPECT_TRUE(queue.empty());
}

TEST(QuicAckHandlerTest, AckedSentFrameUpdatesCongestionAndRtt) {
    fiber::quic::QuicConnection::Options options = fiber::test::quic_options();
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

TEST(QuicAckHandlerTest, HandlesDescendingAckRangesWithSingleReverseScan) {
    fiber::quic::QuicConnection connection(fiber::test::quic_options());
    auto &space = connection.packet_number_space(fiber::quic::QuicEncryptionLevel::Initial);

    for (std::uint64_t packet_number = 0; packet_number <= 6; ++packet_number) {
        fiber::quic::QuicOutputFrame *frame = space.alloc_frame();
        ASSERT_NE(frame, nullptr);
        frame->type = fiber::quic::QuicFrameType::Ping;
        frame->packet_number = packet_number;
        frame->packet_len = 100;
        frame->send_time = fiber::quic::QuicTime{10 + static_cast<std::int64_t>(packet_number)};
        frame->packet_ack_eliciting = true;
        space.sent_frames.push_back(*frame);
        fiber::quic::quic_congestion_on_packet_sent(connection.congestion(), frame->packet_len, true, false);
    }
    // Model a long-lived packet number space so the interleaved unacknowledged
    // packets are not immediately declared lost by the packet threshold.
    space.next_packet_number = 100;

    constexpr std::uint8_t ranges[]{0, 0, 0, 0}; // ACK singleton packet ranges 4 and 2 after 6.
    fiber::quic::QuicInputFrame ack{};
    ack.type = fiber::quic::QuicFrameType::Ack;
    ack.level = fiber::quic::QuicEncryptionLevel::Initial;
    ack.u.ack.largest = 6;
    ack.u.ack.range_count = 2;
    ack.u.ack.first_range = 0;
    ack.data = {ranges, sizeof(ranges)};

    auto result = fiber::quic::quic_handle_ack_frame(connection, fiber::quic::QuicEncryptionLevel::Initial, ack,
                                                     fiber::quic::QuicTime{100});

    ASSERT_TRUE(result.has_value()) << static_cast<int>(result.error());
    EXPECT_TRUE(result->acked_frames);
    EXPECT_EQ(connection.congestion().in_flight, 400U);

    constexpr std::array<std::uint64_t, 4> remaining{0, 1, 3, 5};
    fiber::quic::QuicOutputFrame *previous = nullptr;
    fiber::quic::QuicOutputFrame *frame = space.sent_frames.front();
    for (std::uint64_t packet_number: remaining) {
        ASSERT_NE(frame, nullptr);
        EXPECT_EQ(frame->packet_number, packet_number);
        EXPECT_EQ(space.sent_frames.prev_of(*frame), previous);
        previous = frame;
        frame = space.sent_frames.next_of(*frame);
    }
    EXPECT_EQ(frame, nullptr);
    ASSERT_NE(space.sent_frames.back(), nullptr);
    EXPECT_EQ(space.sent_frames.back()->packet_number, 5U);
}

TEST(QuicAckHandlerTest, MissingAckEcnDisablesPathEcn) {
    fiber::quic::QuicConnection::Options options = fiber::test::quic_options();
    options.role = fiber::quic::QuicConnectionRole::Server;
    fiber::quic::QuicConnection connection(options);
    auto *path = connection.active_path();
    ASSERT_NE(path, nullptr);
    path->ecn_state = fiber::quic::QuicEcnState::Testing;
    path->ecn_validation_sent = 1;

    auto &space = connection.packet_number_space(fiber::quic::QuicEncryptionLevel::Application);
    space.ecn_sent_counters.ect0 = 1;
    fiber::quic::QuicOutputFrame *frame = space.alloc_frame();
    ASSERT_NE(frame, nullptr);
    frame->type = fiber::quic::QuicFrameType::Ping;
    frame->packet_number = 0;
    frame->packet_len = 1200;
    frame->send_time = fiber::quic::QuicTime{10};
    frame->packet_ack_eliciting = true;
    frame->packet_ecn = fiber::net::UdpEcn::Ect0;
    frame->packet_path_seqnum = path->seqnum;
    frame->packet_ecn_validation_probe = true;
    space.next_packet_number = 1;
    space.sent_frames.push_back(*frame);
    fiber::quic::quic_congestion_on_packet_sent(connection.congestion(), 1200, true, false);

    fiber::quic::QuicInputFrame ack{};
    ack.type = fiber::quic::QuicFrameType::Ack;
    ack.level = fiber::quic::QuicEncryptionLevel::Application;
    ack.u.ack.largest = 0;
    ack.u.ack.first_range = 0;

    auto result = fiber::quic::quic_handle_ack_frame(connection, fiber::quic::QuicEncryptionLevel::Application, ack,
                                                     fiber::quic::QuicTime{90});

    ASSERT_TRUE(result.has_value()) << static_cast<int>(result.error());
    EXPECT_EQ(path->ecn_state, fiber::quic::QuicEcnState::Failed);
}

TEST(QuicAckHandlerTest, AckEcnValidatesTestingPath) {
    fiber::quic::QuicConnection connection(fiber::test::quic_options());
    auto *path = connection.active_path();
    ASSERT_NE(path, nullptr);
    path->ecn_state = fiber::quic::QuicEcnState::Testing;
    path->ecn_validation_sent = 1;

    auto &space = connection.packet_number_space(fiber::quic::QuicEncryptionLevel::Application);
    space.ecn_sent_counters.ect0 = 1;
    fiber::quic::QuicOutputFrame *frame = space.alloc_frame();
    ASSERT_NE(frame, nullptr);
    frame->type = fiber::quic::QuicFrameType::Ping;
    frame->packet_number = 0;
    frame->packet_len = 1200;
    frame->send_time = fiber::quic::QuicTime{10};
    frame->packet_ack_eliciting = true;
    frame->packet_ecn = fiber::net::UdpEcn::Ect0;
    frame->packet_path_seqnum = path->seqnum;
    frame->packet_ecn_validation_probe = true;
    space.next_packet_number = 1;
    space.sent_frames.push_back(*frame);
    fiber::quic::quic_congestion_on_packet_sent(connection.congestion(), 1200, true, false);

    fiber::quic::QuicInputFrame ack{};
    ack.type = fiber::quic::QuicFrameType::AckEcn;
    ack.level = fiber::quic::QuicEncryptionLevel::Application;
    ack.u.ack.largest = 0;
    ack.u.ack.first_range = 0;
    ack.u.ack.ect0 = 1;

    auto result = fiber::quic::quic_handle_ack_frame(connection, fiber::quic::QuicEncryptionLevel::Application, ack,
                                                     fiber::quic::QuicTime{90});

    ASSERT_TRUE(result.has_value()) << static_cast<int>(result.error());
    EXPECT_EQ(path->ecn_state, fiber::quic::QuicEcnState::Capable);
    EXPECT_EQ(path->ecn_validation_acked, 1U);
    EXPECT_EQ(space.peer_ecn_counters.ect0, 1U);
}

TEST(QuicAckHandlerTest, AckEcnCeTriggersCongestionResponse) {
    fiber::quic::QuicConnection connection(fiber::test::quic_options());
    auto *path = connection.active_path();
    ASSERT_NE(path, nullptr);
    path->ecn_state = fiber::quic::QuicEcnState::Testing;
    path->ecn_validation_sent = 1;

    auto &space = connection.packet_number_space(fiber::quic::QuicEncryptionLevel::Application);
    space.ecn_sent_counters.ect0 = 1;
    fiber::quic::QuicOutputFrame *frame = space.alloc_frame();
    ASSERT_NE(frame, nullptr);
    frame->type = fiber::quic::QuicFrameType::Ping;
    frame->packet_number = 0;
    frame->packet_len = 1200;
    frame->send_time = fiber::quic::QuicTime{10};
    frame->packet_ack_eliciting = true;
    frame->packet_ecn = fiber::net::UdpEcn::Ect0;
    frame->packet_path_seqnum = path->seqnum;
    frame->packet_ecn_validation_probe = true;
    space.next_packet_number = 1;
    space.sent_frames.push_back(*frame);
    fiber::quic::quic_congestion_on_packet_sent(connection.congestion(), 1200, true, false);

    fiber::quic::QuicInputFrame ack{};
    ack.type = fiber::quic::QuicFrameType::AckEcn;
    ack.level = fiber::quic::QuicEncryptionLevel::Application;
    ack.u.ack.largest = 0;
    ack.u.ack.first_range = 0;
    ack.u.ack.ce = 1;

    auto result = fiber::quic::quic_handle_ack_frame(connection, fiber::quic::QuicEncryptionLevel::Application, ack,
                                                     fiber::quic::QuicTime{90});

    ASSERT_TRUE(result.has_value()) << static_cast<int>(result.error());
    EXPECT_EQ(path->ecn_state, fiber::quic::QuicEcnState::Capable);
    EXPECT_LT(connection.congestion().window, 13200U);
    EXPECT_EQ(connection.congestion().recovery_start, fiber::quic::QuicTime{90});
    EXPECT_EQ(space.peer_ecn_counters.ce, 1U);
}

TEST(QuicLossRecoveryTest, LostEcnValidationProbeDisablesPathEcn) {
    fiber::quic::QuicConnection connection(fiber::test::quic_options());
    auto *path = connection.active_path();
    ASSERT_NE(path, nullptr);
    path->ecn_state = fiber::quic::QuicEcnState::Testing;
    path->ecn_validation_sent = 1;

    auto &space = connection.packet_number_space(fiber::quic::QuicEncryptionLevel::Application);
    fiber::quic::QuicOutputFrame *frame = space.alloc_frame();
    ASSERT_NE(frame, nullptr);
    frame->type = fiber::quic::QuicFrameType::Ping;
    frame->packet_number = 0;
    frame->packet_len = 1200;
    frame->send_time = fiber::quic::QuicTime{0};
    frame->packet_ack_eliciting = true;
    frame->packet_ecn = fiber::net::UdpEcn::Ect0;
    frame->packet_path_seqnum = path->seqnum;
    frame->packet_ecn_validation_probe = true;
    space.next_packet_number = 4;
    space.largest_acked_packet_number = 3;
    space.sent_frames.push_back(*frame);
    fiber::quic::quic_congestion_on_packet_sent(connection.congestion(), 1200, true, false);

    auto result = fiber::quic::quic_detect_lost(connection, fiber::quic::QuicTime{400}, nullptr);

    ASSERT_TRUE(result.has_value()) << static_cast<int>(result.error());
    EXPECT_EQ(path->ecn_state, fiber::quic::QuicEcnState::Failed);
}

TEST(QuicLossRecoveryTest, SelectsPtoTimerFromLatestSentPacket) {
    fiber::quic::QuicConnection connection(fiber::test::quic_options());
    auto &space = connection.packet_number_space(fiber::quic::QuicEncryptionLevel::Initial);

    fiber::quic::QuicOutputFrame *frame = space.alloc_frame();
    ASSERT_NE(frame, nullptr);
    frame->type = fiber::quic::QuicFrameType::Ping;
    frame->packet_number = 0;
    frame->send_time = fiber::quic::QuicTime{10};
    frame->packet_ack_eliciting = true;
    space.next_packet_number = 1;
    space.sent_frames.push_back(*frame);

    const auto timer = fiber::quic::quic_loss_detection_timer(connection, fiber::quic::QuicTime{10}, 0);

    EXPECT_EQ(timer.mode, fiber::quic::QuicLossTimerMode::Pto);
    EXPECT_EQ(timer.delay, fiber::quic::QuicTime{997});
}

TEST(QuicLossRecoveryTest, LossTimerTakesPriorityOverPto) {
    fiber::quic::QuicConnection connection(fiber::test::quic_options());
    auto &space = connection.packet_number_space(fiber::quic::QuicEncryptionLevel::Initial);

    fiber::quic::QuicOutputFrame *frame = space.alloc_frame();
    ASSERT_NE(frame, nullptr);
    frame->type = fiber::quic::QuicFrameType::Ping;
    frame->packet_number = 0;
    frame->send_time = fiber::quic::QuicTime{10};
    frame->packet_ack_eliciting = true;
    space.next_packet_number = 4;
    space.largest_acked_packet_number = 3;
    space.sent_frames.push_back(*frame);

    const auto timer = fiber::quic::quic_loss_detection_timer(connection, fiber::quic::QuicTime{10}, 0);

    EXPECT_EQ(timer.mode, fiber::quic::QuicLossTimerMode::Lost);
    EXPECT_EQ(timer.delay, fiber::quic::QuicTime{0});
}

TEST(QuicLossRecoveryTest, PtoQueuesTwoCongestionIgnoringPingProbes) {
    fiber::quic::QuicConnection connection(fiber::test::quic_options());
    auto &space = connection.packet_number_space(fiber::quic::QuicEncryptionLevel::Initial);

    fiber::quic::QuicOutputFrame *sent = space.alloc_frame();
    ASSERT_NE(sent, nullptr);
    sent->type = fiber::quic::QuicFrameType::Ping;
    sent->packet_number = 0;
    sent->send_time = fiber::quic::QuicTime{0};
    sent->packet_ack_eliciting = true;
    space.next_packet_number = 1;
    space.sent_frames.push_back(*sent);

    fiber::quic::QuicOutputFrame *normal = space.alloc_frame();
    ASSERT_NE(normal, nullptr);
    normal->type = fiber::quic::QuicFrameType::MaxData;
    normal->u.max_data.max_data = 1024;
    space.pending_frames.push_back(*normal);

    auto queued = fiber::quic::quic_queue_pto_probe_frames(connection, fiber::quic::QuicTime{997}, 0);

    ASSERT_TRUE(queued.has_value()) << static_cast<int>(queued.error());
    EXPECT_TRUE(*queued);

    std::size_t count = 0;
    std::size_t ping_count = 0;
    for (fiber::quic::QuicOutputFrame *frame = space.pending_frames.front(); frame != nullptr;
         frame = space.pending_frames.next_of(*frame)) {
        if (count < 2) {
            EXPECT_EQ(frame->type, fiber::quic::QuicFrameType::Ping);
            EXPECT_TRUE(frame->ignore_congestion);
            ++ping_count;
        }
        ++count;
    }
    EXPECT_EQ(ping_count, 2U);
    EXPECT_EQ(count, 3U);
}
