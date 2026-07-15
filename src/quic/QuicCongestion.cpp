#include "QuicCongestion.h"

#include <algorithm>
#include <cstdlib>

#include "../common/Assert.h"

namespace fiber::quic {

namespace {

[[nodiscard]] std::size_t initial_window() noexcept {
    return std::min<std::size_t>(10 * kQuicCongestionMinInitialSize,
                                 std::max<std::size_t>(2 * kQuicCongestionMinInitialSize, 14720));
}

[[nodiscard]] std::int64_t abs_i64(std::int64_t value) noexcept { return value < 0 ? -value : value; }

[[nodiscard]] QuicTime max_time(QuicTime a, QuicTime b) noexcept { return a > b ? a : b; }

[[nodiscard]] QuicTime abs_time_diff(QuicTime a, QuicTime b) noexcept { return QuicTime{abs_i64((a - b).count())}; }

void subtract_in_flight(QuicCongestionState &cg, std::size_t packet_len) noexcept {
    FIBER_ASSERT(cg.in_flight >= packet_len);
    cg.in_flight -= packet_len;
}

void enter_recovery(QuicCongestionState &cg, std::size_t reduction_basis, QuicTime now, std::size_t path_mtu) noexcept {
    cg.mtu = path_mtu;
    cg.recovery_start = now;
    cg.w_prior = cg.window;
    cg.w_max = (cg.window < cg.w_max) ? cg.window * (10 + kQuicCongestionCubicBeta) / 20 : cg.window;
    cg.ssthresh = reduction_basis * kQuicCongestionCubicBeta / 10;
    cg.window = std::max(cg.ssthresh, cg.mtu * 2);
    cg.w_est = cg.window;
    cg.k = now + quic_congestion_cubic_time(cg);
    cg.idle_start = now;
}

} // namespace


void quic_rtt_init(QuicRttState &rtt) noexcept {
    rtt.latest_rtt = QuicTime{0};
    rtt.avg_rtt = QuicTime{static_cast<std::int64_t>(kQuicCongestionInitialRttMs)};
    rtt.min_rtt = QuicTime::max();
    rtt.rttvar = QuicTime{static_cast<std::int64_t>(kQuicCongestionInitialRttMs / 2)};
    rtt.first_rtt = QuicTime::max();
}

void quic_rtt_sample(QuicRttState &rtt, QuicTime now, QuicTime send_time, std::uint64_t ack_delay_raw,
                     std::uint64_t ack_delay_exponent, QuicTime max_ack_delay, bool handshake_confirmed) noexcept {
    QuicTime latest = now - send_time;
    if (latest < QuicTime{0}) {
        latest = QuicTime{0};
    }
    rtt.latest_rtt = latest;

    if (rtt.min_rtt == QuicTime::max()) {
        rtt.min_rtt = latest;
        rtt.avg_rtt = latest;
        rtt.rttvar = latest / 2;
        rtt.first_rtt = now;
        return;
    }

    rtt.min_rtt = std::min(rtt.min_rtt, latest);

    std::uint64_t shifted = ack_delay_raw;
    if (ack_delay_exponent < 63) {
        shifted <<= ack_delay_exponent;
    } else {
        shifted = std::numeric_limits<std::uint64_t>::max();
    }
    QuicTime ack_delay{static_cast<std::int64_t>(std::min<std::uint64_t>(
            shifted / 1000, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())))};
    if (handshake_confirmed) {
        ack_delay = std::min(ack_delay, max_ack_delay);
    }

    QuicTime adjusted = latest;
    if (ack_delay <= latest - rtt.min_rtt) {
        adjusted -= ack_delay;
    }

    const QuicTime rttvar_sample = abs_time_diff(rtt.avg_rtt, adjusted);
    rtt.rttvar += (rttvar_sample / 4) - (rtt.rttvar / 4);
    rtt.avg_rtt += (adjusted / 8) - (rtt.avg_rtt / 8);
}

void quic_congestion_init(QuicCongestionState &cg, QuicTime now) noexcept {
    cg = QuicCongestionState{};
    cg.window = initial_window();
    cg.ssthresh = std::numeric_limits<std::size_t>::max();
    cg.mtu = kQuicCongestionMinInitialSize;
    cg.recovery_start = now - QuicTime{1};
}

void quic_congestion_reset_for_path(QuicCongestionState &cg, QuicRttState &rtt, QuicTime now) noexcept {
    quic_congestion_init(cg, now);
    quic_rtt_init(rtt);
}

bool quic_congestion_can_send(const QuicCongestionState &cg, bool ignore_congestion) noexcept {
    return ignore_congestion || cg.in_flight < cg.window;
}

void quic_congestion_on_packet_sent(QuicCongestionState &cg, std::size_t packet_len, bool ack_eliciting,
                                    bool closing) noexcept {
    if (ack_eliciting && !closing) {
        cg.in_flight += packet_len;
    }
}

bool quic_congestion_on_ack(QuicCongestionState &cg, const QuicAckSample &sample, std::uint64_t reset_packet_number,
                            QuicTime now, QuicTime oldest_sent_time) noexcept {
    if (sample.packet_len == 0 || sample.packet_number < reset_packet_number) {
        return false;
    }

    const bool blocked = cg.in_flight >= cg.window;
    subtract_in_flight(cg, sample.packet_len);

    if (now < cg.recovery_start) {
        cg.recovery_start = oldest_sent_time - QuicTime{1};
    }

    if (sample.send_time <= cg.recovery_start || cg.idle) {
        return blocked && cg.in_flight < cg.window;
    }

    if (cg.window < cg.ssthresh) {
        cg.window += sample.packet_len;
        return blocked && cg.in_flight < cg.window;
    }

    const std::size_t w_cubic = quic_congestion_cubic_window(cg, now);

    if (cg.window < cg.w_prior) {
        cg.w_est += static_cast<std::uint64_t>(cg.mtu) * sample.packet_len * 3 * (10 - kQuicCongestionCubicBeta) /
                    (10 + kQuicCongestionCubicBeta) / cg.window;
    } else {
        cg.w_est += static_cast<std::uint64_t>(cg.mtu) * sample.packet_len / cg.window;
    }

    if (w_cubic < cg.w_est) {
        cg.window = cg.w_est;
    } else if (w_cubic > cg.window) {
        if (w_cubic >= cg.window * 3 / 2) {
            cg.window += cg.mtu / 2;
        } else {
            cg.window += static_cast<std::uint64_t>(cg.mtu) * (w_cubic - cg.window) / cg.window;
        }
    }

    return blocked && cg.in_flight < cg.window;
}

bool quic_congestion_on_loss(QuicCongestionState &cg, const QuicLossSample &sample, std::uint64_t reset_packet_number,
                             QuicTime now, std::size_t path_mtu) noexcept {
    if (sample.packet_len == 0 || sample.packet_number < reset_packet_number) {
        return false;
    }

    const bool blocked = cg.in_flight >= cg.window;
    subtract_in_flight(cg, sample.packet_len);

    if (sample.send_time <= cg.recovery_start || sample.ignore_loss) {
        return blocked && cg.in_flight < cg.window;
    }

    enter_recovery(cg, cg.in_flight, now, path_mtu);

    return blocked && cg.in_flight < cg.window;
}

bool quic_congestion_on_ecn_ce(QuicCongestionState &cg, QuicTime sent_time, QuicTime now,
                               std::size_t path_mtu) noexcept {
    const bool blocked = cg.in_flight >= cg.window;
    if (sent_time <= cg.recovery_start) {
        return false;
    }

    enter_recovery(cg, cg.window, now, path_mtu);
    return blocked && cg.in_flight < cg.window;
}

void quic_congestion_on_persistent_congestion(QuicCongestionState &cg, QuicTime oldest_sent_time,
                                              std::size_t path_mtu) noexcept {
    cg.mtu = path_mtu;
    cg.recovery_start = oldest_sent_time - QuicTime{1};
    cg.window = cg.mtu * 2;
}

void quic_congestion_on_idle(QuicCongestionState &cg, bool idle, QuicTime now) noexcept {
    if (cg.window >= cg.ssthresh) {
        if (cg.idle) {
            cg.k += now - cg.idle_start;
        }
        cg.idle_start = now;
    }
    cg.idle = idle;
}

std::size_t quic_congestion_cubic_window(QuicCongestionState &cg, QuicTime now) noexcept {
    quic_congestion_on_idle(cg, cg.idle, now);

    const std::int64_t t = (now - cg.k).count();
    if (t > 1000000) {
        return std::numeric_limits<std::size_t>::max();
    }
    if (t < -1000000) {
        return 0;
    }

    const std::int64_t cc =
            10000000000LL / static_cast<std::int64_t>(cg.mtu) / static_cast<std::int64_t>(kQuicCongestionCubicC);
    std::int64_t w = t * t * t / cc + static_cast<std::int64_t>(cg.w_max);
    if (w < 0) {
        return 0;
    }
    if (static_cast<std::uint64_t>(w) > std::numeric_limits<std::size_t>::max()) {
        return std::numeric_limits<std::size_t>::max();
    }
    return static_cast<std::size_t>(w);
}

QuicTime quic_congestion_cubic_time(const QuicCongestionState &cg) noexcept {
    if (cg.w_max <= cg.window) {
        return QuicTime{0};
    }

    const std::int64_t cc =
            10000000000LL / static_cast<std::int64_t>(cg.mtu) / static_cast<std::int64_t>(kQuicCongestionCubicC);
    const std::int64_t v = static_cast<std::int64_t>(cg.w_max - cg.window) * cc;

    std::int64_t x = 5000;
    for (std::uint8_t n = 1; n <= 10; ++n) {
        const std::int64_t d = (v / x / x - x) / 3;
        x += d;
        if (abs_i64(d) <= 100) {
            break;
        }
    }

    if (x < 0) {
        return QuicTime{0};
    }
    return QuicTime{x};
}

QuicTime quic_loss_time_threshold(const QuicRttState &rtt) noexcept {
    QuicTime threshold = max_time(rtt.latest_rtt, rtt.avg_rtt);
    threshold += threshold / 8;
    return max_time(threshold, QuicTime{1});
}

std::uint64_t quic_loss_packet_threshold(std::uint64_t next_packet_number,
                                         std::uint64_t oldest_sent_packet_number) noexcept {
    std::uint64_t threshold = 0;
    if (next_packet_number >= oldest_sent_packet_number) {
        threshold = (next_packet_number - oldest_sent_packet_number) / 2;
    }
    return threshold <= kQuicCongestionPacketThreshold ? kQuicCongestionPacketThreshold : threshold;
}

QuicTime quic_persistent_congestion_duration(const QuicRttState &rtt, QuicTime max_ack_delay) noexcept {
    return (rtt.avg_rtt + max_time(4 * rtt.rttvar, QuicTime{1}) + max_ack_delay) * kQuicCongestionPersistentThreshold;
}

QuicTime quic_pto(const QuicRttState &rtt, QuicTime max_ack_delay, bool application_level,
                  bool handshake_confirmed) noexcept {
    QuicTime duration = rtt.avg_rtt + max_time(4 * rtt.rttvar, QuicTime{1});
    if (application_level && handshake_confirmed) {
        duration += max_ack_delay;
    }
    return duration;
}

} // namespace fiber::quic
