#ifndef FIBER_QUIC_QUIC_PACKET_NUMBER_SPACE_H
#define FIBER_QUIC_QUIC_PACKET_NUMBER_SPACE_H

#include <array>
#include <cstddef>
#include <cstdint>

#include "../net/UdpPacket.h"
#include "QuicCongestion.h"
#include "QuicDataReassembler.h"
#include "QuicFrame.h"

namespace fiber::quic {

inline constexpr std::size_t kQuicPacketNumberSpaceCount = 3;
inline constexpr std::size_t kQuicMaxAckRanges = 32;

struct QuicAckRange {
    std::uint64_t gap = 0;
    std::uint64_t range = 0;
};

struct QuicEcnCounters {
    std::uint64_t ect0 = 0;
    std::uint64_t ect1 = 0;
    std::uint64_t ce = 0;

    [[nodiscard]] bool any() const noexcept { return ect0 != 0 || ect1 != 0 || ce != 0; }
};

struct QuicPacketNumberSpace {
    QuicPacketNumberSpace() noexcept;
    ~QuicPacketNumberSpace();

    void set_frame_pool(QuicOutputFramePool &pool) noexcept;
    void reset(QuicEncryptionLevel space_level) noexcept;

    // Record a received packet number — updates largest_received_packet_number.
    void record_received_packet_number(std::uint64_t packet_number) noexcept;

    // Track a received packet number and build ACK ranges (RFC 9000 Section 13.2.1).
    // Mirrors nginx's ngx_quic_ack_packet().
    //
    // Also updates pending_ack for ack-eliciting packets (before overflow checks),
    // matching nginx's pending_ack update at lines 1206-1221.
    //
    // When ack_eliciting is true and a range overflow or too-old condition occurs,
    // a forced ACK frame is created and queued BEFORE the ranges are modified,
    // exactly matching nginx's behaviour (ngx_quic_ack_packet calls ngx_quic_send_ack
    // before shifting the ranges array, and ngx_quic_send_ack_range for too-old packets).
    // After a forced ACK on overflow, pending_ack is cleared if it was not updated
    // by the current packet (matching nginx's prev_pending == ctx->pending_ack check).
    void on_packet_received(std::uint64_t packet_number, QuicTime received_time, bool ack_eliciting,
                            net::UdpEcn ecn = net::UdpEcn::Unspecified) noexcept;

    // Record an acked packet number (largest acked by peer).
    void record_acked_packet_number(std::uint64_t packet_number) noexcept;

    void record_ecn(net::UdpEcn ecn) noexcept;

    // Drop ACK ranges up to and including pn (nginx's ngx_quic_drop_ack_ranges).
    // Called when an ACK frame we previously sent is itself acknowledged.
    void drop_ack_ranges(std::uint64_t pn) noexcept;

    [[nodiscard]] QuicOutputFrame *alloc_frame() noexcept;
    void release_frame(QuicOutputFrame &frame) noexcept;

    QuicEncryptionLevel level;

    std::uint64_t crypto_sent = 0;
    QuicDataReassembler crypto_recv{};

    std::uint64_t next_packet_number = 0;
    std::uint64_t largest_acked_packet_number = 0;
    std::uint64_t largest_received_packet_number = 0;

    QuicOutputFrameQueue pending_frames{};
    QuicOutputFrameQueue sending_frames{};
    QuicOutputFrameQueue sent_frames{};
    QuicOutputFrame ack_frame{};
    QuicOutputFramePool *frame_pool = nullptr;

    // ACK range tracking (RFC 9000 Section 13.2.1).
    std::uint64_t pending_ack = 0;
    std::uint64_t largest_range = 0;
    std::uint64_t first_range = 0;
    QuicTime largest_received_time{0};
    QuicTime ack_delay_start{0};
    std::uint32_t ack_range_count = 0;
    std::array<QuicAckRange, kQuicMaxAckRanges> ack_ranges{};
    QuicEcnCounters ecn_counters{};
    QuicEcnCounters ecn_sent_counters{};
    QuicEcnCounters peer_ecn_counters{};
    std::uint32_t send_ack_count = 0;
    bool send_ack = false;

private:
    // Build and queue a forced ACK frame with the given range state.
    // Mirrors ngx_quic_send_ack (overflow) or ngx_quic_send_ack_range (too-old).
    void generate_forced_ack(std::uint64_t largest, std::uint64_t first_range_val, std::uint32_t range_count,
                             const QuicAckRange *ranges, QuicTime now, std::uint64_t ack_delay_exponent = 3) noexcept;
};

[[nodiscard]] common::IoResult<void> quic_prepare_ack_frame(QuicOutputFrame &frame, std::uint64_t largest,
                                                            std::uint64_t delay, std::uint32_t range_count,
                                                            std::uint64_t first_range, const QuicAckRange *ranges,
                                                            const QuicEcnCounters &ecn_counters) noexcept;

struct QuicPacketNumberSpaceSnapshot {
    std::uint64_t next_packet_number = 0;
};

// Consolidated ack-eliciting packet handling. Replaces inline field manipulation
// previously scattered across QuicPacketProcessor.cpp.
void quic_record_ack_eliciting_received(QuicPacketNumberSpace &space, std::uint64_t packet_number,
                                        QuicTime received_time, std::uint64_t previous_largest_received) noexcept;

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_PACKET_NUMBER_SPACE_H
