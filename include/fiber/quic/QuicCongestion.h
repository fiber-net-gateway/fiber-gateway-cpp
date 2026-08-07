#ifndef FIBER_QUIC_QUIC_CONGESTION_H
#define FIBER_QUIC_QUIC_CONGESTION_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace fiber::quic {

using QuicTime = std::chrono::milliseconds;

inline constexpr std::size_t kQuicCongestionMinInitialSize = 1200;
inline constexpr std::size_t kQuicCongestionInitialRttMs = 333;
inline constexpr std::size_t kQuicCongestionPacketThreshold = 3;
inline constexpr std::size_t kQuicCongestionPersistentThreshold = 3;
inline constexpr std::size_t kQuicCongestionCubicBeta = 7;
inline constexpr std::size_t kQuicCongestionCubicC = 4;

struct QuicRttState {
    QuicTime latest_rtt{0};
    QuicTime avg_rtt{static_cast<std::int64_t>(kQuicCongestionInitialRttMs)};
    QuicTime min_rtt{QuicTime::max()};
    QuicTime rttvar{static_cast<std::int64_t>(kQuicCongestionInitialRttMs / 2)};
    QuicTime first_rtt{QuicTime::max()};
};

struct QuicCongestionState {
    std::size_t in_flight = 0;
    std::size_t window = 0;
    std::size_t ssthresh = std::numeric_limits<std::size_t>::max();
    std::size_t w_max = 0;
    std::size_t w_est = 0;
    std::size_t w_prior = 0;
    std::size_t mtu = kQuicCongestionMinInitialSize;
    QuicTime recovery_start{-1};
    QuicTime idle_start{0};
    QuicTime k{0};
    bool idle = false;
};

struct QuicAckSample {
    std::size_t packet_len = 0;
    std::uint64_t packet_number = 0;
    QuicTime send_time{0};
};

struct QuicLossSample {
    std::size_t packet_len = 0;
    std::uint64_t packet_number = 0;
    QuicTime send_time{0};
    bool ignore_loss = false;
};

[[nodiscard]] inline QuicTime quic_time_ms(std::chrono::steady_clock::time_point time) noexcept {
    return std::chrono::duration_cast<QuicTime>(time.time_since_epoch());
}

void quic_rtt_init(QuicRttState &rtt) noexcept;
void quic_rtt_sample(QuicRttState &rtt, QuicTime now, QuicTime send_time, std::uint64_t ack_delay_raw,
                     std::uint64_t ack_delay_exponent, QuicTime max_ack_delay, bool handshake_confirmed) noexcept;

void quic_congestion_init(QuicCongestionState &cg, QuicTime now) noexcept;
void quic_congestion_reset_for_path(QuicCongestionState &cg, QuicRttState &rtt, QuicTime now) noexcept;
[[nodiscard]] bool quic_congestion_can_send(const QuicCongestionState &cg, bool ignore_congestion) noexcept;
void quic_congestion_on_packet_sent(QuicCongestionState &cg, std::size_t packet_len, bool ack_eliciting,
                                    bool closing) noexcept;
[[nodiscard]] bool quic_congestion_on_ack(QuicCongestionState &cg, const QuicAckSample &sample,
                                          std::uint64_t reset_packet_number, QuicTime now,
                                          QuicTime oldest_sent_time) noexcept;
[[nodiscard]] bool quic_congestion_on_loss(QuicCongestionState &cg, const QuicLossSample &sample,
                                           std::uint64_t reset_packet_number, QuicTime now,
                                           std::size_t path_mtu) noexcept;
[[nodiscard]] bool quic_congestion_on_ecn_ce(QuicCongestionState &cg, QuicTime sent_time, QuicTime now,
                                             std::size_t path_mtu) noexcept;
void quic_congestion_on_persistent_congestion(QuicCongestionState &cg, QuicTime oldest_sent_time,
                                              std::size_t path_mtu) noexcept;
void quic_congestion_on_idle(QuicCongestionState &cg, bool idle, QuicTime now) noexcept;

[[nodiscard]] std::size_t quic_congestion_cubic_window(QuicCongestionState &cg, QuicTime now) noexcept;
[[nodiscard]] QuicTime quic_congestion_cubic_time(const QuicCongestionState &cg) noexcept;
[[nodiscard]] QuicTime quic_loss_time_threshold(const QuicRttState &rtt) noexcept;
[[nodiscard]] std::uint64_t quic_loss_packet_threshold(std::uint64_t next_packet_number,
                                                       std::uint64_t oldest_sent_packet_number) noexcept;
[[nodiscard]] QuicTime quic_persistent_congestion_duration(const QuicRttState &rtt, QuicTime max_ack_delay) noexcept;
[[nodiscard]] QuicTime quic_pto(const QuicRttState &rtt, QuicTime max_ack_delay, bool application_level,
                                bool handshake_confirmed) noexcept;

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_CONGESTION_H
