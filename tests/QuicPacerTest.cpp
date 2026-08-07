#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>

#include <fiber/quic/QuicPacer.h>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

fiber::quic::QuicPacingOptions exact_options() {
    fiber::quic::QuicPacingOptions options{};
    options.rate_numerator = 1;
    options.rate_denominator = 1;
    options.max_burst_packets = 2;
    options.timer_granularity = 0us;
    return options;
}

fiber::quic::QuicCongestionState congestion_with_window(std::size_t window) {
    fiber::quic::QuicCongestionState congestion{};
    congestion.window = window;
    congestion.mtu = 1200;
    return congestion;
}

fiber::quic::QuicRttState rtt_of(std::chrono::milliseconds value) {
    fiber::quic::QuicRttState rtt{};
    rtt.avg_rtt = value;
    return rtt;
}

TEST(QuicPacerTest, InitialBurstThenReturnsDeadline) {
    auto options = exact_options();
    auto congestion = congestion_with_window(12000);
    auto rtt = rtt_of(100ms);
    fiber::quic::QuicPacerState pacer{};
    const Clock::time_point start{1000ms};

    EXPECT_TRUE(fiber::quic::quic_pacer_check(pacer, options, congestion, rtt, 1200, start).ready);
    fiber::quic::quic_pacer_on_datagram_sent(pacer, 1200);
    EXPECT_TRUE(fiber::quic::quic_pacer_check(pacer, options, congestion, rtt, 1200, start).ready);
    fiber::quic::quic_pacer_on_datagram_sent(pacer, 1200);

    const auto blocked = fiber::quic::quic_pacer_check(pacer, options, congestion, rtt, 1200, start);
    EXPECT_FALSE(blocked.ready);
    EXPECT_EQ(blocked.deadline, start + 10ms);
}

TEST(QuicPacerTest, RefillsAtConfiguredRate) {
    auto options = exact_options();
    auto congestion = congestion_with_window(12000);
    auto rtt = rtt_of(100ms);
    fiber::quic::QuicPacerState pacer{};
    const Clock::time_point start{1000ms};

    ASSERT_TRUE(fiber::quic::quic_pacer_check(pacer, options, congestion, rtt, 1200, start).ready);
    fiber::quic::quic_pacer_on_datagram_sent(pacer, 2400);

    EXPECT_FALSE(fiber::quic::quic_pacer_check(pacer, options, congestion, rtt, 1200, start + 9ms).ready);
    EXPECT_TRUE(fiber::quic::quic_pacer_check(pacer, options, congestion, rtt, 1200, start + 10ms).ready);
}

TEST(QuicPacerTest, IdleCreditIsCappedAtBurstCapacity) {
    auto options = exact_options();
    auto congestion = congestion_with_window(12000);
    auto rtt = rtt_of(100ms);
    fiber::quic::QuicPacerState pacer{};
    const Clock::time_point start{1000ms};

    ASSERT_TRUE(fiber::quic::quic_pacer_check(pacer, options, congestion, rtt, 1200, start).ready);
    fiber::quic::quic_pacer_on_datagram_sent(pacer, 2400);
    ASSERT_TRUE(fiber::quic::quic_pacer_check(pacer, options, congestion, rtt, 1200, start + 1s).ready);
    fiber::quic::quic_pacer_on_datagram_sent(pacer, 1200);
    ASSERT_TRUE(fiber::quic::quic_pacer_check(pacer, options, congestion, rtt, 1200, start + 1s).ready);
    fiber::quic::quic_pacer_on_datagram_sent(pacer, 1200);

    EXPECT_FALSE(fiber::quic::quic_pacer_check(pacer, options, congestion, rtt, 1200, start + 1s).ready);
}

TEST(QuicPacerTest, FractionalRefillsAccumulateAcrossFrequentChecks) {
    auto options = exact_options();
    options.max_burst_packets = 1;
    auto congestion = congestion_with_window(1200);
    auto rtt = rtt_of(1s);
    fiber::quic::QuicPacerState pacer{};
    const Clock::time_point start{1000ms};

    ASSERT_TRUE(fiber::quic::quic_pacer_check(pacer, options, congestion, rtt, 1200, start).ready);
    fiber::quic::quic_pacer_on_datagram_sent(pacer, 1200);
    fiber::quic::QuicPacingDecision decision{};
    for (std::size_t i = 1; i < 1000; ++i) {
        decision = fiber::quic::quic_pacer_check(pacer, options, congestion, rtt, 1200, start + i * 1ms);
    }

    EXPECT_FALSE(decision.ready);
    EXPECT_TRUE(fiber::quic::quic_pacer_check(pacer, options, congestion, rtt, 1200, start + 1s).ready);
}

TEST(QuicPacerTest, LargerWindowAndLowerRttIncreaseRate) {
    auto options = exact_options();
    fiber::quic::QuicPacerState slow{};
    fiber::quic::QuicPacerState fast{};
    const Clock::time_point start{1000ms};

    auto slow_congestion = congestion_with_window(12000);
    auto fast_congestion = congestion_with_window(24000);
    auto slow_rtt = rtt_of(100ms);
    auto fast_rtt = rtt_of(50ms);

    ASSERT_TRUE(fiber::quic::quic_pacer_check(slow, options, slow_congestion, slow_rtt, 1200, start).ready);
    ASSERT_TRUE(fiber::quic::quic_pacer_check(fast, options, fast_congestion, fast_rtt, 1200, start).ready);
    fiber::quic::quic_pacer_on_datagram_sent(slow, 2400);
    fiber::quic::quic_pacer_on_datagram_sent(fast, 2400);

    const auto slow_wait = fiber::quic::quic_pacer_check(slow, options, slow_congestion, slow_rtt, 1200, start);
    const auto fast_wait = fiber::quic::quic_pacer_check(fast, options, fast_congestion, fast_rtt, 1200, start);
    ASSERT_FALSE(slow_wait.ready);
    ASSERT_FALSE(fast_wait.ready);
    EXPECT_LT(fast_wait.deadline, slow_wait.deadline);
}

TEST(QuicPacerTest, ResetAndDisabledModesAreImmediatelyReady) {
    auto options = exact_options();
    auto congestion = congestion_with_window(12000);
    auto rtt = rtt_of(100ms);
    fiber::quic::QuicPacerState pacer{};
    const Clock::time_point start{1000ms};

    ASSERT_TRUE(fiber::quic::quic_pacer_check(pacer, options, congestion, rtt, 1200, start).ready);
    fiber::quic::quic_pacer_on_datagram_sent(pacer, 2400);
    ASSERT_FALSE(fiber::quic::quic_pacer_check(pacer, options, congestion, rtt, 1200, start).ready);

    fiber::quic::quic_pacer_reset(pacer);
    EXPECT_TRUE(fiber::quic::quic_pacer_check(pacer, options, congestion, rtt, 1200, start).ready);

    options.enabled = false;
    fiber::quic::quic_pacer_on_datagram_sent(pacer, std::numeric_limits<std::size_t>::max());
    EXPECT_TRUE(fiber::quic::quic_pacer_check(pacer, options, congestion, rtt, 1200, start).ready);
}

TEST(QuicPacerTest, TimerDeadlineRoundsUpToGranularity) {
    auto options = exact_options();
    options.timer_granularity = 1ms;
    auto congestion = congestion_with_window(12000);
    auto rtt = rtt_of(95ms);
    fiber::quic::QuicPacerState pacer{};
    const Clock::time_point start{1000ms};

    ASSERT_TRUE(fiber::quic::quic_pacer_check(pacer, options, congestion, rtt, 1200, start).ready);
    fiber::quic::quic_pacer_on_datagram_sent(pacer, 2400);
    const auto blocked = fiber::quic::quic_pacer_check(pacer, options, congestion, rtt, 1200, start);

    ASSERT_FALSE(blocked.ready);
    EXPECT_EQ(blocked.deadline, start + 10ms);
}

TEST(QuicPacerTest, DefaultGranularitySupportsSubMillisecondDeadlines) {
    fiber::quic::QuicPacingOptions options{};
    auto congestion = congestion_with_window(12000);
    auto rtt = rtt_of(1ms);
    fiber::quic::QuicPacerState pacer{};
    const Clock::time_point start{1000ms};

    ASSERT_TRUE(fiber::quic::quic_pacer_check(pacer, options, congestion, rtt, 1200, start).ready);
    fiber::quic::quic_pacer_on_datagram_sent(pacer, 12000);
    const auto blocked = fiber::quic::quic_pacer_check(pacer, options, congestion, rtt, 1200, start);

    ASSERT_FALSE(blocked.ready);
    EXPECT_EQ(blocked.deadline, start + 100us);
}

} // namespace
