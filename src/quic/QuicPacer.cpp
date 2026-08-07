#include <fiber/quic/QuicPacer.h>

#include <algorithm>
#include <limits>

namespace fiber::quic {

namespace {

inline constexpr std::uint64_t kNanosPerSecond = 1000000000ULL;
inline constexpr std::int64_t kMinPacingRttMs = 1;
inline constexpr std::int64_t kMaxPacingRttMs = 60000;

[[nodiscard]] std::uint64_t saturating_mul(std::uint64_t left, std::uint64_t right) noexcept {
    if (left == 0 || right == 0) {
        return 0;
    }
    if (left > std::numeric_limits<std::uint64_t>::max() / right) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return left * right;
}

[[nodiscard]] std::uint64_t saturating_add(std::uint64_t left, std::uint64_t right) noexcept {
    if (left > std::numeric_limits<std::uint64_t>::max() - right) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return left + right;
}

[[nodiscard]] std::uint64_t ceil_div(std::uint64_t numerator, std::uint64_t denominator) noexcept {
    if (denominator == 0) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return numerator / denominator + static_cast<std::uint64_t>(numerator % denominator != 0);
}

[[nodiscard]] std::uint64_t pacing_rate(const QuicPacingOptions &options, const QuicCongestionState &congestion,
                                        std::size_t path_mtu, const QuicRttState &rtt) noexcept {
    const std::uint64_t mtu = std::max<std::uint64_t>(path_mtu, 1);
    const std::uint64_t window = std::max<std::uint64_t>(congestion.window, mtu);
    const std::int64_t rtt_ms = std::clamp<std::int64_t>(rtt.avg_rtt.count(), kMinPacingRttMs, kMaxPacingRttMs);

    const long double numerator = static_cast<long double>(window) * options.rate_numerator * 1000.0L;
    const long double denominator = static_cast<long double>(rtt_ms) * options.rate_denominator;
    const long double rate = denominator > 0.0L ? numerator / denominator : 0.0L;
    if (rate >= static_cast<long double>(std::numeric_limits<std::uint64_t>::max())) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return std::max<std::uint64_t>(static_cast<std::uint64_t>(rate), 1);
}

[[nodiscard]] std::size_t pacing_capacity(const QuicPacingOptions &options, const QuicCongestionState &congestion,
                                          std::size_t path_mtu) noexcept {
    const std::uint64_t mtu = std::max<std::uint64_t>(path_mtu, 1);
    const std::uint64_t burst = saturating_mul(mtu, options.max_burst_packets);
    const std::uint64_t window = std::max<std::uint64_t>(congestion.window, mtu);
    const std::uint64_t capacity = std::max<std::uint64_t>(mtu, std::min(window, burst));
    return static_cast<std::size_t>(std::min<std::uint64_t>(capacity, std::numeric_limits<std::int64_t>::max()));
}

void refill_budget(QuicPacerState &state, std::chrono::steady_clock::time_point now) noexcept {
    if (now <= state.updated_at) {
        return;
    }
    if (state.budget_bytes >= state.capacity_bytes) {
        state.updated_at = now;
        state.refill_remainder = 0;
        return;
    }

    const std::int64_t elapsed_signed =
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - state.updated_at).count();
    state.updated_at = now;
    if (elapsed_signed <= 0) {
        return;
    }

    const std::uint64_t elapsed = static_cast<std::uint64_t>(elapsed_signed);
    const std::uint64_t elapsed_whole = elapsed / kNanosPerSecond;
    const std::uint64_t elapsed_remainder = elapsed % kNanosPerSecond;
    const std::uint64_t rate_whole = state.rate_bytes_per_second / kNanosPerSecond;
    const std::uint64_t rate_remainder = state.rate_bytes_per_second % kNanosPerSecond;

    std::uint64_t added = saturating_mul(elapsed_whole, state.rate_bytes_per_second);
    added = saturating_add(added, saturating_mul(elapsed_remainder, rate_whole));
    const std::uint64_t fractional = elapsed_remainder * rate_remainder + state.refill_remainder;
    added = saturating_add(added, fractional / kNanosPerSecond);

    const std::size_t needed = state.capacity_bytes - state.budget_bytes;
    if (added >= needed) {
        state.budget_bytes = state.capacity_bytes;
        state.refill_remainder = 0;
        return;
    }

    state.budget_bytes += static_cast<std::size_t>(added);
    state.refill_remainder = fractional % kNanosPerSecond;
}

[[nodiscard]] std::uint64_t round_wait(std::uint64_t wait_ns, std::chrono::microseconds granularity) noexcept {
    if (granularity.count() <= 0) {
        return wait_ns;
    }
    const auto granularity_signed = std::chrono::duration_cast<std::chrono::nanoseconds>(granularity).count();
    if (granularity_signed <= 0) {
        return wait_ns;
    }
    const std::uint64_t quantum = static_cast<std::uint64_t>(granularity_signed);
    return saturating_mul(ceil_div(wait_ns, quantum), quantum);
}

} // namespace

void quic_pacer_reset(QuicPacerState &state) noexcept { state = QuicPacerState{}; }

QuicPacingDecision quic_pacer_check(QuicPacerState &state, const QuicPacingOptions &options,
                                    const QuicCongestionState &congestion, const QuicRttState &rtt,
                                    std::size_t path_mtu, std::chrono::steady_clock::time_point now) noexcept {
    if (!options.enabled) {
        return QuicPacingDecision{true, now};
    }

    const std::size_t mtu = std::max<std::size_t>(path_mtu, 1);
    const std::size_t capacity = pacing_capacity(options, congestion, mtu);
    const std::uint64_t rate = pacing_rate(options, congestion, mtu, rtt);

    if (!state.initialized) {
        state.updated_at = now;
        state.budget_bytes = capacity;
        state.rate_bytes_per_second = rate;
        state.capacity_bytes = capacity;
        state.initialized = true;
    } else {
        refill_budget(state, now);
        state.capacity_bytes = capacity;
        state.rate_bytes_per_second = rate;
        state.budget_bytes = std::min(state.budget_bytes, capacity);
    }

    if (state.budget_bytes >= mtu) {
        return QuicPacingDecision{true, now};
    }

    const std::uint64_t deficit = mtu - state.budget_bytes;
    std::uint64_t wait_numerator = saturating_mul(deficit, kNanosPerSecond);
    if (wait_numerator > state.refill_remainder) {
        wait_numerator -= state.refill_remainder;
    } else {
        wait_numerator = 1;
    }
    std::uint64_t wait_ns = ceil_div(wait_numerator, state.rate_bytes_per_second);
    wait_ns = std::max<std::uint64_t>(wait_ns, 1);
    wait_ns = round_wait(wait_ns, options.timer_granularity);
    const std::uint64_t max_duration = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    wait_ns = std::min(wait_ns, max_duration);
    return QuicPacingDecision{false, now + std::chrono::nanoseconds{static_cast<std::int64_t>(wait_ns)}};
}

void quic_pacer_on_datagram_sent(QuicPacerState &state, std::size_t wire_bytes) noexcept {
    if (!state.initialized || wire_bytes == 0) {
        return;
    }
    state.budget_bytes = wire_bytes >= state.budget_bytes ? 0 : state.budget_bytes - wire_bytes;
}

} // namespace fiber::quic
