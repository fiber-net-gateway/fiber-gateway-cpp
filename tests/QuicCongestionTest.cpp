#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <ranges>

#include <fiber/quic/QuicCongestion.h>
#include <fiber/quic/QuicConnection.h>
#include "quic/QuicAckHandler.h"
#include "quic/QuicLossRecovery.h"

#include "QuicCubicVectors.h"
#include "QuicTestLoop.h"

namespace cubickat = fiber::quic::cubickat;

namespace {

void prepare_persistent_congestion_loss(fiber::quic::QuicConnection &connection) {
    fiber::quic::QuicRttState &rtt = connection.rtt();
    rtt.latest_rtt = fiber::quic::QuicTime{10};
    rtt.avg_rtt = fiber::quic::QuicTime{10};
    rtt.min_rtt = fiber::quic::QuicTime{10};
    rtt.rttvar = fiber::quic::QuicTime{1};
    rtt.first_rtt = fiber::quic::QuicTime{50};

    auto &space = connection.packet_number_space(fiber::quic::QuicEncryptionLevel::Application);
    constexpr std::array send_times{fiber::quic::QuicTime{100}, fiber::quic::QuicTime{300}};
    for (std::size_t i = 0; i < send_times.size(); ++i) {
        fiber::quic::QuicOutputFrame *frame = space.alloc_frame();
        ASSERT_NE(frame, nullptr);
        frame->type = fiber::quic::QuicFrameType::Ping;
        frame->packet_number = i;
        frame->packet_len = 1200;
        frame->send_time = send_times[i];
        frame->packet_ack_eliciting = true;
        space.sent_frames.push_back(*frame);
    }
    space.next_packet_number = 5;
    space.largest_acked_packet_number = 4;

    // Keep the ordinary CUBIC loss response above the minimum window so the
    // tests can distinguish it from persistent-congestion collapse.
    connection.congestion().in_flight = 12000;
}

void detect_loss_with_ack_interval(fiber::quic::QuicConnection &connection, fiber::quic::QuicTime oldest_acked,
                                   fiber::quic::QuicTime newest_acked) {
    const fiber::quic::QuicLossAckStat stat{
            .oldest = oldest_acked,
            .newest = newest_acked,
    };
    auto lost = fiber::quic::quic_detect_lost(connection, fiber::quic::QuicTime{500}, &stat);
    ASSERT_TRUE(lost.has_value()) << static_cast<int>(lost.error());
}

// The canonical post-loss state shared by the CUBIC KAT vectors: init at t=0,
// 12000 bytes in flight, one 1200-byte packet lost at t=100. Leaves
// window = ssthresh = 7560, w_max = w_prior = 12000, cg.k = 100 + K.
void canonical_post_loss(fiber::quic::QuicCongestionState &cg) {
    fiber::quic::quic_congestion_init(cg, fiber::quic::QuicTime{0});
    fiber::quic::quic_congestion_on_packet_sent(cg, 12000, true, false);
    (void) fiber::quic::quic_congestion_on_loss(cg,
                                                fiber::quic::QuicLossSample{.packet_len = 1200,
                                                                            .packet_number = 0,
                                                                            .send_time = fiber::quic::QuicTime{0},
                                                                            .ignore_loss = false},
                                                0, fiber::quic::QuicTime{100}, 1200);
}

} // namespace

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
    const bool unblocked = fiber::quic::quic_congestion_on_ack(
            cg,
            fiber::quic::QuicAckSample{.packet_len = 1200, .packet_number = 0, .send_time = fiber::quic::QuicTime{0}},
            0, fiber::quic::QuicTime{50}, fiber::quic::QuicTime{0});

    EXPECT_FALSE(unblocked);
    EXPECT_EQ(cg.in_flight, 0U);
    EXPECT_EQ(cg.window, 13200U);
}

TEST(QuicCongestionTest, LossUsesNginxMultiplicativeDecrease) {
    fiber::quic::QuicCongestionState cg{};
    fiber::quic::quic_congestion_init(cg, fiber::quic::QuicTime{0});
    fiber::quic::quic_congestion_on_packet_sent(cg, 12000, true, false);

    const bool unblocked =
            fiber::quic::quic_congestion_on_loss(cg,
                                                 fiber::quic::QuicLossSample{.packet_len = 1200,
                                                                             .packet_number = 0,
                                                                             .send_time = fiber::quic::QuicTime{0},
                                                                             .ignore_loss = false},
                                                 0, fiber::quic::QuicTime{100}, 1200);

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

TEST(QuicCongestionTest, RttSampleClampsAckDelayAfterHandshakeConfirmation) {
    fiber::quic::QuicRttState rtt{};
    fiber::quic::quic_rtt_init(rtt);

    fiber::quic::quic_rtt_sample(rtt, fiber::quic::QuicTime{40}, fiber::quic::QuicTime{0}, 0, 3,
                                 fiber::quic::QuicTime{25}, false);
    fiber::quic::quic_rtt_sample(rtt, fiber::quic::QuicTime{100}, fiber::quic::QuicTime{0}, 60000, 0,
                                 fiber::quic::QuicTime{25}, true);

    EXPECT_EQ(rtt.latest_rtt, fiber::quic::QuicTime{100});
    EXPECT_EQ(rtt.min_rtt, fiber::quic::QuicTime{40});
    EXPECT_EQ(rtt.avg_rtt, fiber::quic::QuicTime{44});
    EXPECT_EQ(rtt.rttvar, fiber::quic::QuicTime{23});
}

TEST(QuicCongestionTest, RttSampleSubtractsAckDelayAtMinRttBoundary) {
    fiber::quic::QuicRttState rtt{};
    fiber::quic::quic_rtt_init(rtt);

    fiber::quic::quic_rtt_sample(rtt, fiber::quic::QuicTime{40}, fiber::quic::QuicTime{0}, 0, 3,
                                 fiber::quic::QuicTime{25}, false);
    fiber::quic::quic_rtt_sample(rtt, fiber::quic::QuicTime{65}, fiber::quic::QuicTime{0}, 25000, 0,
                                 fiber::quic::QuicTime{25}, true);

    EXPECT_EQ(rtt.latest_rtt, fiber::quic::QuicTime{65});
    EXPECT_EQ(rtt.min_rtt, fiber::quic::QuicTime{40});
    EXPECT_EQ(rtt.avg_rtt, fiber::quic::QuicTime{40});
    EXPECT_EQ(rtt.rttvar, fiber::quic::QuicTime{15});
}

TEST(QuicCongestionTest, CubicWindowMatchesRfc9438Curve) {
    // KAT against RFC 9438 4.1: W_cubic(t) = C*(t-K)^3 + W_max with C = 0.4 in
    // byte units, t in ms. Vectors are generated and cross-checked against the
    // closed form by tests/quic_kat/gen_cubic_kat.py before emission.
    fiber::quic::QuicCongestionState cg{};
    canonical_post_loss(cg);

    EXPECT_EQ(cg.k.count(), 100 + cubickat::kCanonicalKMs);

    for (const cubickat::CubicCurvePoint &point: cubickat::kCurveA) {
        const std::size_t window =
                fiber::quic::quic_congestion_cubic_window(cg, cg.k + fiber::quic::QuicTime{point.t_ms});
        EXPECT_EQ(window, point.window) << "curve at t offset " << point.t_ms << " ms";
    }
}

TEST(QuicCongestionTest, CubicWindowScalesWithMtuAndWmax) {
    fiber::quic::QuicCongestionState cg{};
    cg.mtu = 1500;
    cg.w_max = 90000;
    cg.ssthresh = 63000;
    cg.window = 63000;
    cg.k = fiber::quic::QuicTime{cubickat::kCurveBEpochMs};

    for (const cubickat::CubicCurvePoint &point: cubickat::kCurveB) {
        const std::size_t window =
                fiber::quic::quic_congestion_cubic_window(cg, cg.k + fiber::quic::QuicTime{point.t_ms});
        EXPECT_EQ(window, point.window) << "curve at t offset " << point.t_ms << " ms";
    }
}

TEST(QuicCongestionTest, CubicTimeSolvesRfc9438K) {
    // RFC 9438 4.2: K = cbrt(W_max*(1-beta)/C). Cases with w_max <= window
    // (no epoch offset) solve to 0.
    for (const cubickat::CubicKCase &kase: cubickat::kKSolves) {
        fiber::quic::QuicCongestionState cg{};
        cg.mtu = kase.mtu;
        cg.w_max = kase.w_max;
        cg.window = kase.window;
        EXPECT_EQ(fiber::quic::quic_congestion_cubic_time(cg).count(), kase.k_ms)
                << "K for w_max " << kase.w_max << " window " << kase.window;
    }
}

TEST(QuicCongestionTest, CubicWindowFastRampCapsIncrementWhenCurveFarAhead) {
    // A single ack long after the loss sees W_cubic >= 1.5 * window, so the
    // avoidance increase is capped at mtu/2 instead of scaling with the gap.
    fiber::quic::QuicCongestionState cg{};
    canonical_post_loss(cg);

    const fiber::quic::QuicTime now = cg.k + fiber::quic::QuicTime{cubickat::kFastRampOffsetMs};
    fiber::quic::quic_congestion_on_packet_sent(cg, 1200, true, false);
    (void) fiber::quic::quic_congestion_on_ack(cg,
                                               fiber::quic::QuicAckSample{.packet_len = 1200,
                                                                          .packet_number = 1,
                                                                          .send_time = now - fiber::quic::QuicTime{50}},
                                               0, now, now - fiber::quic::QuicTime{50});

    EXPECT_EQ(cg.window, cubickat::kFastRampWindow); // 7560 + mtu/2
    EXPECT_EQ(cg.w_est, cubickat::kFastRampWEst);
}

TEST(QuicCongestionTest, CubicWindowClampsOutsideTimeDomain) {
    fiber::quic::QuicCongestionState cg{};
    cg.mtu = 1200;
    cg.w_max = 12000;
    cg.ssthresh = 7560;
    cg.window = 7560;

    EXPECT_EQ(fiber::quic::quic_congestion_cubic_window(cg, fiber::quic::QuicTime{1000001}),
              std::numeric_limits<std::size_t>::max());
    EXPECT_EQ(fiber::quic::quic_congestion_cubic_window(cg, fiber::quic::QuicTime{-1000001}), 0U);
}

TEST(QuicCongestionTest, CongestionAvoidanceTrickleRidesCubicChaseAndRampCap) {
    // One 1200 B packet acked per 50 ms, in_flight never refilled: the Reno
    // estimate stays behind the curve, so the window rides the cubic chase
    // branch and finally the mtu/2 fast-ramp cap.
    fiber::quic::QuicCongestionState cg{};
    canonical_post_loss(cg);
    const fiber::quic::QuicTime epoch_start = cg.k;

    std::size_t checkpoint = 0;
    for (std::uint32_t acks = 1; acks <= cubickat::kTrickleAcks; ++acks) {
        const fiber::quic::QuicTime now{cubickat::kTrickleFirstNowMs + (cubickat::kTrickleCadenceMs * (acks - 1))};
        fiber::quic::quic_congestion_on_packet_sent(cg, 1200, true, false);
        (void) fiber::quic::quic_congestion_on_ack(
                cg,
                fiber::quic::QuicAckSample{
                        .packet_len = 1200, .packet_number = acks, .send_time = now - fiber::quic::QuicTime{50}},
                0, now, now - fiber::quic::QuicTime{50});
        if (acks % 10 != 0) {
            continue;
        }
        ASSERT_LT(checkpoint, cubickat::kTrickleRun.size());
        const cubickat::CubicAvoidCheckpoint &cp = cubickat::kTrickleRun[checkpoint++];
        EXPECT_EQ(cg.window, cp.window) << "window after " << acks << " acks";
        EXPECT_EQ(cg.w_est, cp.w_est) << "w_est after " << acks << " acks";
        EXPECT_EQ(cg.window == cg.w_est, cp.window_is_w_est) << "region after " << acks << " acks";
    }
    EXPECT_EQ(checkpoint, cubickat::kTrickleRun.size());
    EXPECT_EQ(cg.k, epoch_start); // idle == false must never shift the epoch
    EXPECT_GT(cg.window, cg.w_max); // past K the window exceeds W_max
}

TEST(QuicCongestionTest, CongestionAvoidanceSaturatingVisitsTcpFriendlyRegion) {
    // in_flight refilled to the window before each 10 ms ack: the Reno slope
    // now exceeds the curve slope around t=0, so the run enters the
    // TCP-friendly region (window == w_est, RFC 9438 4.1.2) and crosses back
    // onto the cubic curve past K.
    fiber::quic::QuicCongestionState cg{};
    canonical_post_loss(cg);
    const fiber::quic::QuicTime epoch_start = cg.k;

    std::size_t checkpoint = 0;
    for (std::uint32_t acks = 1; acks <= cubickat::kSaturatingAcks; ++acks) {
        const fiber::quic::QuicTime now{cubickat::kSaturatingFirstNowMs +
                                        (cubickat::kSaturatingCadenceMs * (acks - 1))};
        while (cg.in_flight + 1200 <= cg.window) {
            fiber::quic::quic_congestion_on_packet_sent(cg, 1200, true, false);
        }
        (void) fiber::quic::quic_congestion_on_ack(
                cg,
                fiber::quic::QuicAckSample{
                        .packet_len = 1200, .packet_number = acks, .send_time = now - fiber::quic::QuicTime{50}},
                0, now, now - fiber::quic::QuicTime{50});
        if (acks % 50 != 0) {
            continue;
        }
        ASSERT_LT(checkpoint, cubickat::kSaturatingRun.size());
        const cubickat::CubicAvoidCheckpoint &cp = cubickat::kSaturatingRun[checkpoint++];
        EXPECT_EQ(cg.window, cp.window) << "window after " << acks << " acks";
        EXPECT_EQ(cg.w_est, cp.w_est) << "w_est after " << acks << " acks";
        EXPECT_EQ(cg.window == cg.w_est, cp.window_is_w_est) << "region after " << acks << " acks";
    }
    EXPECT_EQ(checkpoint, cubickat::kSaturatingRun.size());
    EXPECT_EQ(cg.k, epoch_start); // idle == false must never shift the epoch
    EXPECT_EQ(cg.w_max, 12000U);
    EXPECT_EQ(cg.w_prior, 12000U);
    EXPECT_GT(cg.window, cg.w_max); // past K the window exceeds W_max
}

TEST(QuicCongestionTest, IdleShiftsCubicEpochAcrossTheIdleSpan) {
    // App-limited epoch anchoring: while the connection idles in congestion
    // avoidance, every cubic_window() evaluation (and the idle->active
    // transition) advances k by the idle time elapsed, so the curve freezes at
    // the value it had when the idle period began. Idle time never advances
    // the CUBIC clock.
    fiber::quic::QuicCongestionState cg{};
    canonical_post_loss(cg); // window == ssthresh == 7560: avoidance
    const fiber::quic::QuicTime epoch = cg.k;

    fiber::quic::quic_congestion_on_idle(cg, true, fiber::quic::QuicTime{300});
    EXPECT_TRUE(cg.idle);
    EXPECT_EQ(cg.k, epoch); // entering idle shifts nothing by itself
    EXPECT_EQ(cg.idle_start, fiber::quic::QuicTime{300});

    const std::size_t frozen = fiber::quic::quic_congestion_cubic_window(cg, fiber::quic::QuicTime{500});
    EXPECT_EQ(cg.k, epoch + fiber::quic::QuicTime{200}); // shifted by the idle chunk
    // The frozen value is the mirror curve of a never-idle state at the
    // moment the idle period began.
    fiber::quic::QuicCongestionState mirror{};
    canonical_post_loss(mirror);
    EXPECT_EQ(frozen, fiber::quic::quic_congestion_cubic_window(mirror, fiber::quic::QuicTime{300}));

    // Still idle: each evaluation re-anchors to its own idle_start.
    EXPECT_EQ(fiber::quic::quic_congestion_cubic_window(cg, fiber::quic::QuicTime{800}), frozen);
    EXPECT_EQ(cg.k, epoch + fiber::quic::QuicTime{500});

    // Going active closes out the final idle chunk...
    fiber::quic::quic_congestion_on_idle(cg, false, fiber::quic::QuicTime{1000});
    EXPECT_EQ(cg.k, epoch + fiber::quic::QuicTime{700}); // the full 700 ms idle span
    EXPECT_FALSE(cg.idle);
    EXPECT_EQ(fiber::quic::quic_congestion_cubic_window(cg, fiber::quic::QuicTime{1000}), frozen);

    // ...and active evaluations never shift again.
    (void) fiber::quic::quic_congestion_cubic_window(cg, fiber::quic::QuicTime{2000});
    EXPECT_EQ(cg.k, epoch + fiber::quic::QuicTime{700});
}

TEST(QuicCongestionTest, IdleDuringSlowStartDoesNotAnchorEpoch) {
    // The epoch shift only applies in avoidance (window >= ssthresh): during
    // slow start there is no CUBIC epoch to protect, so idle_start is not
    // even tracked. enter_recovery() re-anchors it at the first loss.
    fiber::quic::QuicCongestionState cg{};
    fiber::quic::quic_congestion_init(cg, fiber::quic::QuicTime{0});

    fiber::quic::quic_congestion_on_idle(cg, true, fiber::quic::QuicTime{500});
    EXPECT_TRUE(cg.idle);
    EXPECT_EQ(cg.idle_start, fiber::quic::QuicTime{0}); // guard: window < ssthresh
    EXPECT_EQ(cg.k, fiber::quic::QuicTime{0});

    (void) fiber::quic::quic_congestion_cubic_window(cg, fiber::quic::QuicTime{700});
    EXPECT_EQ(cg.k, fiber::quic::QuicTime{0});
    EXPECT_EQ(cg.idle_start, fiber::quic::QuicTime{0});
}

TEST(QuicCongestionTest, AckWhileIdleLeavesWindowUntouched) {
    // quic_congestion_on_ack() early-outs while idle: the accounting still
    // lands, but neither the slow-start nor the avoidance branch may grow the
    // window from an ack observed during an idle period.
    fiber::quic::QuicCongestionState cg{};
    canonical_post_loss(cg);
    fiber::quic::quic_congestion_on_packet_sent(cg, 1200, true, false);
    fiber::quic::quic_congestion_on_idle(cg, true, fiber::quic::QuicTime{300});

    (void) fiber::quic::quic_congestion_on_ack(
            cg,
            fiber::quic::QuicAckSample{.packet_len = 1200, .packet_number = 1, .send_time = fiber::quic::QuicTime{250}},
            0, fiber::quic::QuicTime{400}, fiber::quic::QuicTime{250});

    EXPECT_EQ(cg.in_flight, 10800U); // the ack still lands in the accounting
    EXPECT_EQ(cg.window, 7560U); // but no branch grew the window
    EXPECT_EQ(cg.w_est, 7560U);
}

TEST(QuicCongestionTest, ResetForPathRestoresInitialWindowAndRtt) {
    // quic_congestion_reset_for_path (path migration / rebind): the whole
    // CUBIC epoch, the negotiated MTU and the RTT estimate are dropped; the
    // path starts over in slow start at the initial window. The connection
    // wrapper re-counts in-flight bytes for packets above the reset threshold
    // afterwards (pinned by PathResetKeepsOtherSpacesInFlightForTheirAcks).
    fiber::quic::QuicCongestionState cg{};
    fiber::quic::QuicRttState rtt{};
    fiber::quic::quic_congestion_init(cg, fiber::quic::QuicTime{0});
    fiber::quic::quic_rtt_sample(rtt, fiber::quic::QuicTime{100}, fiber::quic::QuicTime{20}, 0, 3,
                                 fiber::quic::QuicTime{25}, false);
    fiber::quic::quic_congestion_on_packet_sent(cg, 12000, true, false);
    (void) fiber::quic::quic_congestion_on_loss(cg,
                                                fiber::quic::QuicLossSample{.packet_len = 1200,
                                                                            .packet_number = 0,
                                                                            .send_time = fiber::quic::QuicTime{0},
                                                                            .ignore_loss = false},
                                                0, fiber::quic::QuicTime{100}, 1200);
    // Leave avoidance-shaped state behind: negotiated MTU, in-flight bytes,
    // idle epoch anchor.
    cg.mtu = 1500;
    cg.in_flight = 6000;
    cg.idle = true;
    cg.idle_start = fiber::quic::QuicTime{500};

    fiber::quic::quic_congestion_reset_for_path(cg, rtt, fiber::quic::QuicTime{1000});

    EXPECT_EQ(cg.window, 12000U); // initial_window(): back to slow start
    EXPECT_EQ(cg.ssthresh, std::numeric_limits<std::size_t>::max());
    EXPECT_EQ(cg.mtu, fiber::quic::kQuicCongestionMinInitialSize); // MTU is re-discovered per path
    EXPECT_EQ(cg.in_flight, 0U);
    EXPECT_EQ(cg.w_max, 0U);
    EXPECT_EQ(cg.w_est, 0U);
    EXPECT_EQ(cg.w_prior, 0U);
    EXPECT_EQ(cg.k, fiber::quic::QuicTime{0});
    EXPECT_EQ(cg.idle_start, fiber::quic::QuicTime{0});
    EXPECT_FALSE(cg.idle);
    EXPECT_EQ(cg.recovery_start, fiber::quic::QuicTime{999});

    EXPECT_EQ(rtt.latest_rtt, fiber::quic::QuicTime{0});
    EXPECT_EQ(rtt.avg_rtt, fiber::quic::QuicTime{fiber::quic::kQuicCongestionInitialRttMs});
    EXPECT_EQ(rtt.min_rtt, fiber::quic::QuicTime::max());
    EXPECT_EQ(rtt.rttvar, fiber::quic::QuicTime{fiber::quic::kQuicCongestionInitialRttMs / 2});
    EXPECT_EQ(rtt.first_rtt, fiber::quic::QuicTime::max());
}

TEST(QuicOutputFrameListTest, MaintainsReverseLinksAcrossMutations) {
    fiber::quic::QuicOutputFrameList queue{};
    fiber::quic::QuicOutputFrameList prefix{};
    fiber::quic::QuicOutputFrame first{};
    fiber::quic::QuicOutputFrame second{};
    fiber::quic::QuicOutputFrame third{};
    fiber::quic::QuicOutputFrame fourth{};
    fiber::quic::QuicOutputFrame fifth{};

    queue.push_back(first);
    queue.push_back(second);
    queue.push_front(third);
    queue.insert_after(first, fourth);
    queue.erase(fourth);

    prefix.push_back(fourth);
    prefix.push_back(fifth);
    queue.splice_front(prefix);

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
    for (auto *it: std::ranges::reverse_view(expected)) {
        ASSERT_EQ(frame, it);
        EXPECT_EQ(queue.next_of(*frame), next);
        next = frame;
        frame = queue.prev_of(*frame);
    }
    EXPECT_EQ(frame, nullptr);

    fiber::quic::QuicOutputFrame *removed_tail = queue.pop_back();
    ASSERT_EQ(removed_tail, &second);
    EXPECT_EQ(queue.back(), &first);
    EXPECT_EQ(queue.next_of(first), nullptr);
    EXPECT_EQ(removed_tail->hook.next, &removed_tail->hook);
    EXPECT_EQ(removed_tail->hook.prev, &removed_tail->hook);
    EXPECT_FALSE(removed_tail->hook.linked());
    queue.push_back(*removed_tail);

    for (auto *i: expected) {
        fiber::quic::QuicOutputFrame *removed = queue.pop_front();
        ASSERT_EQ(removed, i);
        EXPECT_EQ(removed->hook.next, &removed->hook);
        EXPECT_EQ(removed->hook.prev, &removed->hook);
        EXPECT_FALSE(removed->hook.linked());
    }
    EXPECT_TRUE(queue.empty());
}

TEST(QuicAckHandlerTest, AckedSentFrameUpdatesCongestionAndRtt) {
    fiber::quic::QuicConnection::Options options = fiber::test::quic_options();
    options.role = fiber::quic::QuicConnectionRole::Server;
    fiber::quic::QuicConnection connection(fiber::test::quic_endpoint(), options);
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

// A path reset (migration) during the handshake zeroes bytes in flight but
// sets reset_packet_number from the Application space only. Handshake
// packets still in flight whose numbers clear that threshold are subtracted
// when acked: the reset must keep them counted, or the ACK underflows the
// counter (a peer could abort the process by rebinding mid-handshake). Found
// by fuzz/quic_network_fuzzer.
TEST(QuicAckHandlerTest, PathResetKeepsOtherSpacesInFlightForTheirAcks) {
    fiber::quic::QuicConnection::Options options = fiber::test::quic_options();
    options.role = fiber::quic::QuicConnectionRole::Server;
    fiber::quic::QuicConnection connection(fiber::test::quic_endpoint(), options);
    auto &handshake = connection.packet_number_space(fiber::quic::QuicEncryptionLevel::Handshake);
    for (std::uint64_t pn = 0; pn < 6; ++pn) {
        fiber::quic::QuicOutputFrame *frame = handshake.alloc_frame();
        ASSERT_NE(frame, nullptr);
        frame->type = fiber::quic::QuicFrameType::Ping;
        frame->packet_number = pn;
        frame->packet_len = 100;
        frame->send_time = fiber::quic::QuicTime{10};
        frame->packet_ack_eliciting = true;
        handshake.sent_frames.push_back(*frame);
        fiber::quic::quic_congestion_on_packet_sent(connection.congestion(), 100, true, false);
    }
    handshake.next_packet_number = 6;
    connection.packet_number_space(fiber::quic::QuicEncryptionLevel::Application).next_packet_number = 4;

    connection.reset_congestion_for_path(fiber::quic::QuicTime{20});
    ASSERT_EQ(connection.reset_packet_number(), 4U);
    // Handshake 4 and 5 clear the threshold and stay counted; 0..3 do not.
    EXPECT_EQ(connection.congestion().in_flight, 200U);

    fiber::quic::QuicInputFrame ack{};
    ack.type = fiber::quic::QuicFrameType::Ack;
    ack.level = fiber::quic::QuicEncryptionLevel::Handshake;
    ack.u.ack.largest = 5;
    ack.u.ack.first_range = 5;
    auto result = fiber::quic::quic_handle_ack_frame(connection, fiber::quic::QuicEncryptionLevel::Handshake, ack,
                                                     fiber::quic::QuicTime{30});
    ASSERT_TRUE(result.has_value()) << static_cast<int>(result.error());
    EXPECT_TRUE(handshake.sent_frames.empty());
    EXPECT_EQ(connection.congestion().in_flight, 0U);
}

TEST(QuicAckHandlerTest, AckOfAckDropsRangesThroughSentAckLargest) {
    fiber::quic::QuicConnection connection(fiber::test::quic_endpoint(), fiber::test::quic_options());
    auto &space = connection.packet_number_space(fiber::quic::QuicEncryptionLevel::Initial);

    space.on_packet_received(10, fiber::quic::QuicTime{1}, true);

    fiber::quic::QuicOutputFrame *sent_ack = space.alloc_frame();
    ASSERT_NE(sent_ack, nullptr);
    sent_ack->type = fiber::quic::QuicFrameType::Ack;
    sent_ack->u.ack.largest = 10;
    sent_ack->packet_number = 100;
    sent_ack->send_time = fiber::quic::QuicTime{10};
    // ACK is not ack-eliciting by itself, but the packet carrying it can be
    // ack-eliciting when another frame is coalesced into the same packet.
    sent_ack->packet_ack_eliciting = true;
    space.sent_frames.push_back(*sent_ack);
    space.next_packet_number = 101;

    // This packet arrived after the ACK above was sent and must remain tracked
    // when the peer acknowledges local packet 100.
    space.on_packet_received(11, fiber::quic::QuicTime{2}, true);
    space.send_ack = true;
    ASSERT_EQ(space.largest_range, 11U);
    ASSERT_EQ(space.first_range, 1U);
    ASSERT_EQ(space.pending_ack, 11U);

    fiber::quic::QuicInputFrame ack_of_ack{};
    ack_of_ack.type = fiber::quic::QuicFrameType::Ack;
    ack_of_ack.level = fiber::quic::QuicEncryptionLevel::Initial;
    ack_of_ack.u.ack.largest = 100;
    ack_of_ack.u.ack.first_range = 0;

    auto result = fiber::quic::quic_handle_ack_frame(connection, fiber::quic::QuicEncryptionLevel::Initial, ack_of_ack,
                                                     fiber::quic::QuicTime{20});

    ASSERT_TRUE(result.has_value()) << static_cast<int>(result.error());
    EXPECT_TRUE(result->acked_frames);
    EXPECT_EQ(space.largest_range, 11U);
    EXPECT_EQ(space.first_range, 0U);
    EXPECT_EQ(space.ack_range_count, 0U);
    EXPECT_EQ(space.pending_ack, 11U);
}

TEST(QuicAckHandlerTest, HandlesDescendingAckRangesWithSingleReverseScan) {
    fiber::quic::QuicConnection connection(fiber::test::quic_endpoint(), fiber::test::quic_options());
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
    ack.data = {.data = ranges, .len = sizeof(ranges)};

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
    fiber::quic::QuicConnection connection(fiber::test::quic_endpoint(), options);
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
    fiber::quic::QuicConnection connection(fiber::test::quic_endpoint(), fiber::test::quic_options());
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
    fiber::quic::QuicConnection connection(fiber::test::quic_endpoint(), fiber::test::quic_options());
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
    fiber::quic::QuicConnection connection(fiber::test::quic_endpoint(), fiber::test::quic_options());
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

TEST(QuicLossRecoveryTest, AckedIntervalBeforeLostIntervalCanEstablishPersistentCongestion) {
    fiber::quic::QuicConnection connection(fiber::test::quic_endpoint(), fiber::test::quic_options());
    prepare_persistent_congestion_loss(connection);

    detect_loss_with_ack_interval(connection, fiber::quic::QuicTime{10}, fiber::quic::QuicTime{20});

    EXPECT_EQ(connection.congestion().window, 2 * connection.congestion().mtu);
}

TEST(QuicLossRecoveryTest, AckedIntervalAfterLostIntervalCanEstablishPersistentCongestion) {
    fiber::quic::QuicConnection connection(fiber::test::quic_endpoint(), fiber::test::quic_options());
    prepare_persistent_congestion_loss(connection);

    detect_loss_with_ack_interval(connection, fiber::quic::QuicTime{400}, fiber::quic::QuicTime{450});

    EXPECT_EQ(connection.congestion().window, 2 * connection.congestion().mtu);
}

TEST(QuicLossRecoveryTest, AckedPacketInsideLostIntervalPreventsPersistentCongestion) {
    fiber::quic::QuicConnection connection(fiber::test::quic_endpoint(), fiber::test::quic_options());
    prepare_persistent_congestion_loss(connection);

    detect_loss_with_ack_interval(connection, fiber::quic::QuicTime{200}, fiber::quic::QuicTime{200});

    EXPECT_GT(connection.congestion().window, 2 * connection.congestion().mtu);
}

TEST(QuicLossRecoveryTest, SelectsPtoTimerFromLatestSentPacket) {
    fiber::quic::QuicConnection connection(fiber::test::quic_endpoint(), fiber::test::quic_options());
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
    fiber::quic::QuicConnection connection(fiber::test::quic_endpoint(), fiber::test::quic_options());
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
    fiber::quic::QuicConnection connection(fiber::test::quic_endpoint(), fiber::test::quic_options());
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
            EXPECT_TRUE(frame->pto_probe);
            ++ping_count;
        }
        ++count;
    }
    EXPECT_EQ(ping_count, 2U);
    EXPECT_EQ(count, 3U);
}

namespace {

// One unacknowledged Initial packet sent at t=0: the PTO arm of the loss
// detection timer. With the default (sampled-never) RTT state the base PTO
// duration is 333 + 4*166 = 997 ms (quic_pto).
void arm_initial_pto(fiber::quic::QuicConnection &connection) {
    auto &space = connection.packet_number_space(fiber::quic::QuicEncryptionLevel::Initial);

    fiber::quic::QuicOutputFrame *frame = space.alloc_frame();
    ASSERT_NE(frame, nullptr);
    frame->type = fiber::quic::QuicFrameType::Ping;
    frame->packet_number = 0;
    frame->send_time = fiber::quic::QuicTime{0};
    frame->packet_ack_eliciting = true;
    space.next_packet_number = 1;
    space.sent_frames.push_back(*frame);
}

} // namespace

TEST(QuicLossRecoveryTest, PtoDelayDoublesWithEachBackoffRound) {
    // RFC 9002 6.2.1: each consecutive PTO doubles the timer. send_time = 0
    // and now = 0 expose the raw backed-off duration as the timer delay.
    fiber::quic::QuicConnection connection(fiber::test::quic_endpoint(), fiber::test::quic_options());
    arm_initial_pto(connection);

    for (std::uint32_t count = 0; count <= 3; ++count) {
        const auto timer = fiber::quic::quic_loss_detection_timer(connection, fiber::quic::QuicTime{0}, count);
        EXPECT_EQ(timer.mode, fiber::quic::QuicLossTimerMode::Pto) << "pto_count " << count;
        EXPECT_EQ(timer.delay, fiber::quic::QuicTime{997LL << count}) << "pto_count " << count;
    }

    // The doubling saturates at int64 max once the next round would overflow:
    // 997 * 2^53 still fits, every round beyond clamps.
    const auto last_exact = fiber::quic::quic_loss_detection_timer(connection, fiber::quic::QuicTime{0}, 53);
    EXPECT_EQ(last_exact.mode, fiber::quic::QuicLossTimerMode::Pto);
    EXPECT_EQ(last_exact.delay, fiber::quic::QuicTime{997LL << 53});
    for (const std::uint32_t count: {54U, 100U, 1000U}) {
        const auto clamped = fiber::quic::quic_loss_detection_timer(connection, fiber::quic::QuicTime{0}, count);
        EXPECT_EQ(clamped.mode, fiber::quic::QuicLossTimerMode::Pto) << "pto_count " << count;
        EXPECT_EQ(clamped.delay, fiber::quic::QuicTime::max()) << "pto_count " << count;
    }
}

TEST(QuicLossRecoveryTest, PtoProbesWaitForBackedOffDeadline) {
    // The probe path and the timer must agree on the backed-off deadline: one
    // round of backoff moves it from 997 to 2*997 ms, and no probe frames may
    // be queued a millisecond early (audit #2 regression domain).
    fiber::quic::QuicConnection connection(fiber::test::quic_endpoint(), fiber::test::quic_options());
    arm_initial_pto(connection);
    auto &space = connection.packet_number_space(fiber::quic::QuicEncryptionLevel::Initial);

    auto early = fiber::quic::quic_queue_pto_probe_frames(connection, fiber::quic::QuicTime{1993}, 1);
    ASSERT_TRUE(early.has_value()) << static_cast<int>(early.error());
    EXPECT_FALSE(*early);
    EXPECT_TRUE(space.pending_frames.empty());
    EXPECT_EQ(fiber::quic::quic_loss_detection_timer(connection, fiber::quic::QuicTime{1993}, 1).delay,
              fiber::quic::QuicTime{1});

    auto at_deadline = fiber::quic::quic_queue_pto_probe_frames(connection, fiber::quic::QuicTime{1994}, 1);
    ASSERT_TRUE(at_deadline.has_value()) << static_cast<int>(at_deadline.error());
    EXPECT_TRUE(*at_deadline);

    std::size_t probes = 0;
    for (const fiber::quic::QuicOutputFrame *frame = space.pending_frames.front(); frame != nullptr;
         frame = space.pending_frames.next_of(*frame)) {
        EXPECT_EQ(frame->type, fiber::quic::QuicFrameType::Ping);
        EXPECT_TRUE(frame->ignore_congestion);
        EXPECT_TRUE(frame->pto_probe);
        ++probes;
    }
    EXPECT_EQ(probes, 2U);
}
