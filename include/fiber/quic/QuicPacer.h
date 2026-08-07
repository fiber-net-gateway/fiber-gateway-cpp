#ifndef FIBER_QUIC_QUIC_PACER_H
#define FIBER_QUIC_QUIC_PACER_H

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "QuicCongestion.h"

namespace fiber::quic {

struct QuicPacingOptions {
    bool enabled = true;
    std::uint32_t rate_numerator = 5;
    std::uint32_t rate_denominator = 4;
    std::uint16_t max_burst_packets = 10;
    std::chrono::microseconds timer_granularity{100};
};

struct QuicPacerState {
    std::chrono::steady_clock::time_point updated_at{};
    std::size_t budget_bytes = 0;
    std::uint64_t rate_bytes_per_second = 0;
    // Fractional byte credit in billionths of a byte.
    std::uint64_t refill_remainder = 0;
    std::size_t capacity_bytes = 0;
    bool initialized = false;
};

struct QuicPacingDecision {
    bool ready = true;
    std::chrono::steady_clock::time_point deadline{};
};

void quic_pacer_reset(QuicPacerState &state) noexcept;

[[nodiscard]] QuicPacingDecision quic_pacer_check(QuicPacerState &state, const QuicPacingOptions &options,
                                                  const QuicCongestionState &congestion, const QuicRttState &rtt,
                                                  std::size_t path_mtu,
                                                  std::chrono::steady_clock::time_point now) noexcept;

void quic_pacer_on_datagram_sent(QuicPacerState &state, std::size_t wire_bytes) noexcept;

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_PACER_H
