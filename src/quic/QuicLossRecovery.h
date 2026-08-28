#ifndef FIBER_QUIC_QUIC_LOSS_RECOVERY_H
#define FIBER_QUIC_QUIC_LOSS_RECOVERY_H

#include <fiber/common/IoError.h>
#include <fiber/quic/QuicConnection.h>

namespace fiber::quic {

struct QuicLossAckStat {
    QuicTime max_packet_send_time{QuicTime::max()};
    QuicTime oldest{QuicTime::max()};
    QuicTime newest{QuicTime::max()};
    bool max_packet_ack_eliciting = false;
};

struct QuicLossRecoveryResult {
    bool unblocked = false;
    bool lost_frames = false;
    bool force_send = false;
};

struct QuicLossDetectionTimer {
    QuicLossTimerMode mode = QuicLossTimerMode::None;
    QuicTime delay{0};
};

[[nodiscard]] QuicTime quic_oldest_sent_time(QuicConnection &connection) noexcept;

[[nodiscard]] common::IoResult<QuicLossRecoveryResult> quic_detect_lost(QuicConnection &connection, QuicTime now,
                                                                        const QuicLossAckStat *stat) noexcept;

[[nodiscard]] QuicLossDetectionTimer quic_loss_detection_timer(const QuicConnection &connection, QuicTime now,
                                                               std::uint32_t pto_count) noexcept;

[[nodiscard]] common::IoResult<bool> quic_queue_pto_probe_frames(QuicConnection &connection, QuicTime now,
                                                                 std::uint32_t pto_count) noexcept;

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_LOSS_RECOVERY_H
