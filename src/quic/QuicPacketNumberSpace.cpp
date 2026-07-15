#include "QuicPacketNumberSpace.h"

#include <cstring>
#include <new>

#include "QuicCursor.h"
#include "QuicProtocol.h"
#include "QuicTransportCodec.h"

namespace fiber::quic {

namespace {

void fill_ecn_fields(QuicOutputFrame &frame, const QuicEcnCounters &ecn_counters) noexcept {
    if (!ecn_counters.any()) {
        return;
    }

    frame.type = QuicFrameType::AckEcn;
    frame.u.ack.ect0 = ecn_counters.ect0;
    frame.u.ack.ect1 = ecn_counters.ect1;
    frame.u.ack.ce = ecn_counters.ce;
}

} // namespace

QuicPacketNumberSpace::QuicPacketNumberSpace() noexcept { reset(QuicEncryptionLevel::Initial); }

QuicPacketNumberSpace::~QuicPacketNumberSpace() {
    auto release_queue = [this](QuicOutputFrameQueue &queue) noexcept {
        while (QuicOutputFrame *frame = queue.pop_front()) {
            release_frame(*frame);
        }
    };

    release_queue(pending_frames);
    release_queue(sending_frames);
    release_queue(sent_frames);
    quic_output_frame_release_data(ack_frame);
}

void QuicPacketNumberSpace::set_frame_pool(QuicOutputFramePool &pool) noexcept { frame_pool = &pool; }

void QuicPacketNumberSpace::reset(QuicEncryptionLevel space_level) noexcept {
    quic_output_frame_release_data(ack_frame);
    level = space_level;
    crypto_sent = 0;
    crypto_recv.clear();
    next_packet_number = 0;
    largest_acked_packet_number = kUnsetPacketNumber;
    largest_received_packet_number = kUnsetPacketNumber;
    ack_frame = QuicOutputFrame{};
    pending_ack = kUnsetPacketNumber;
    largest_range = kUnsetPacketNumber;
    first_range = 0;
    largest_received_time = QuicTime{0};
    ack_delay_start = QuicTime{0};
    ack_range_count = 0;
    ack_ranges = {};
    ecn_counters = {};
    ecn_sent_counters = {};
    peer_ecn_counters = {};
    send_ack_count = 0;
    send_ack = false;
}

QuicOutputFrame *QuicPacketNumberSpace::alloc_frame() noexcept {
    return frame_pool != nullptr ? frame_pool->alloc() : new (std::nothrow) QuicOutputFrame{};
}

void QuicPacketNumberSpace::release_frame(QuicOutputFrame &frame) noexcept {
    if (&frame == &ack_frame) {
        return;
    }
    if (frame.queued) {
        return;
    }

    if (frame_pool != nullptr) {
        frame_pool->release(&frame);
        return;
    }

    quic_output_frame_release_data(frame);
    delete &frame;
}

void QuicPacketNumberSpace::record_received_packet_number(std::uint64_t packet_number) noexcept {
    if (largest_received_packet_number == kUnsetPacketNumber || packet_number > largest_received_packet_number) {
        largest_received_packet_number = packet_number;
    }
}

void QuicPacketNumberSpace::record_acked_packet_number(std::uint64_t packet_number) noexcept {
    if (largest_acked_packet_number == kUnsetPacketNumber || packet_number > largest_acked_packet_number) {
        largest_acked_packet_number = packet_number;
    }
}

void QuicPacketNumberSpace::record_ecn(net::UdpEcn ecn) noexcept {
    switch (ecn) {
        case net::UdpEcn::Ect0:
            ++ecn_counters.ect0;
            break;
        case net::UdpEcn::Ect1:
            ++ecn_counters.ect1;
            break;
        case net::UdpEcn::Ce:
            ++ecn_counters.ce;
            break;
        case net::UdpEcn::NonEct:
        case net::UdpEcn::Unspecified:
            break;
    }
}

common::IoResult<void> quic_prepare_ack_frame(QuicOutputFrame &frame, std::uint64_t largest, std::uint64_t delay,
                                              std::uint32_t range_count, std::uint64_t first_range,
                                              const QuicAckRange *ranges,
                                              const QuicEcnCounters &ecn_counters) noexcept {
    std::uint8_t range_buf[512];
    std::size_t range_buf_len = 0;
    if (range_count > 0 && ranges != nullptr) {
        QuicWriteCursor rcur(range_buf, sizeof(range_buf));
        for (std::uint32_t i = 0; i < range_count; ++i) {
            auto wrote = quic_write_varint(rcur, ranges[i].gap);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            wrote = quic_write_varint(rcur, ranges[i].range);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
        }
        range_buf_len = rcur.offset();
    }

    quic_output_frame_release_data(frame);
    frame = QuicOutputFrame{};
    frame.type = QuicFrameType::Ack;
    frame.u.ack.largest = largest;
    frame.u.ack.delay = delay;
    frame.u.ack.range_count = range_count;
    frame.u.ack.first_range = first_range;
    fill_ecn_fields(frame, ecn_counters);

    if (range_buf_len > 0) {
        auto set_data = quic_output_frame_set_owned_data(frame, range_buf, range_buf_len);
        if (!set_data) {
            return std::unexpected(set_data.error());
        }
    }

    auto frame_len = quic_output_frame_encoded_len(frame);
    if (!frame_len) {
        return std::unexpected(frame_len.error());
    }

    return {};
}

// ---------------------------------------------------------------------------
// generate_forced_ack — build and queue a forced ACK frame.
//
// Mirrors ngx_quic_send_ack (for overflow) or ngx_quic_send_ack_range (for
// too-old packets).  Uses the static ack_frame member when available; falls
// back to a pool-allocated frame when ack_frame is already queued.
// ---------------------------------------------------------------------------
void QuicPacketNumberSpace::generate_forced_ack(std::uint64_t largest, std::uint64_t first_range_val,
                                                std::uint32_t range_count, const QuicAckRange *ranges, QuicTime now,
                                                std::uint64_t ack_delay_exponent) noexcept {
    if (largest == kUnsetPacketNumber) {
        return;
    }

    // Use the static ack_frame if available; otherwise fall back to pool.
    QuicOutputFrame *frame = ack_frame.queued ? alloc_frame() : &ack_frame;
    if (frame == nullptr) {
        return;
    }

    std::uint64_t ack_delay_us = 0;
    if (level == QuicEncryptionLevel::Application && now > largest_received_time) {
        ack_delay_us = static_cast<std::uint64_t>((now - largest_received_time).count()) * 1000;
        ack_delay_us >>= ack_delay_exponent;
    }

    auto prepared =
            quic_prepare_ack_frame(*frame, largest, ack_delay_us, range_count, first_range_val, ranges, ecn_counters);
    if (!prepared) {
        return;
    }

    pending_frames.push_front(*frame);
}

// ---------------------------------------------------------------------------
// on_packet_received — RFC 9000 Section 13.2.1 ACK range tracking.
// Ported from nginx's ngx_quic_ack_packet (ngx_event_quic_ack.c).
//
// When ack_eliciting is true and a range overflow or too-old condition is
// about to occur, a forced ACK frame is generated with the CURRENT (pre-
// modification) range state, matching nginx's behaviour exactly:
//
//   ngx_quic_ack_packet line 1252-1262:
//     if (ctx->nranges == NGX_QUIC_MAX_RANGES) {
//         if (prev_pending != NGX_QUIC_UNSET_PN)
//             ngx_quic_send_ack(c, ctx);  // ACK with current ranges
//     }
//     ...modify ranges...
//     if (pkt->need_ack)
//         ctx->send_ack = NGX_QUIC_MAX_ACK_GAP;  // force immediate send
//
//   ngx_quic_ack_packet line 1392-1399 (too-old):
//     if (ctx->nranges == NGX_QUIC_MAX_RANGES) {
//         if (pkt->need_ack)
//             return ngx_quic_send_ack_range(c, ctx, pn, pn);
//         return NGX_OK;
//     }
// ---------------------------------------------------------------------------
void QuicPacketNumberSpace::on_packet_received(std::uint64_t pn, QuicTime received_time, bool ack_eliciting,
                                               net::UdpEcn ecn) noexcept {
    // Always track the largest received PN.
    if (largest_received_packet_number == kUnsetPacketNumber || pn > largest_received_packet_number) {
        largest_received_packet_number = pn;
    }

    // Save pending_ack before any modification (mirrors nginx's prev_pending
    // saved at the top of ngx_quic_ack_packet, used to decide whether to
    // clear pending_ack after a forced ACK on range overflow).
    const std::uint64_t prev_pending_ack = pending_ack;

    // Update pending_ack for ack-eliciting packets BEFORE range overflow
    // handling, exactly matching nginx's ngx_quic_ack_packet where the
    // pending_ack update (lines 1206-1221) precedes the range checks.
    if (ack_eliciting) {
        if (pending_ack == kUnsetPacketNumber || pending_ack < pn) {
            pending_ack = pn;
        }
    }

    const std::uint64_t base = largest_range;

    // First packet ever — initialise the range.
    if (base == kUnsetPacketNumber) {
        record_ecn(ecn);
        largest_range = pn;
        largest_received_time = received_time;
        return;
    }

    // Duplicate of current largest — nothing to do.
    if (base == pn) {
        return;
    }

    std::uint64_t largest = base;
    std::uint64_t smallest = largest - first_range;

    // -----------------------------------------------------------------------
    // pn > base: new largest packet.
    // -----------------------------------------------------------------------
    if (pn > base) {
        if (pn - base == 1) {
            // Sequential extension.
            record_ecn(ecn);
            ++first_range;
            largest_range = pn;
            largest_received_time = received_time;
            return;
        }

        // A gap appears before the old largest.  Shift old first_range into
        // ack_ranges[0] and start a fresh first_range at the new PN.
        //
        // If the ranges array is already full, the oldest range will be
        // silently dropped.  For ack-eliciting packets with pending ACK
        // state, generate a forced ACK BEFORE modifying the ranges so it
        // captures the pre-overflow state (mirrors ngx_quic_ack_packet
        // lines 1252-1262 calling ngx_quic_send_ack before the shift).
        if (ack_range_count == kQuicMaxAckRanges && ack_eliciting && prev_pending_ack != kUnsetPacketNumber) {
            generate_forced_ack(largest_range, first_range, ack_range_count, ack_ranges.data(), received_time);
            if (prev_pending_ack == pending_ack) {
                pending_ack = kUnsetPacketNumber;
            }
        }

        record_ecn(ecn);

        const std::uint64_t gap = pn - base - 2;
        const std::uint64_t range = first_range;

        first_range = 0;
        largest_range = pn;
        largest_received_time = received_time;

        // Insert new range at position 0, shifting everything else right.
        if (ack_range_count < kQuicMaxAckRanges) {
            ++ack_range_count;
        }
        if (ack_range_count > 1) {
            std::memmove(&ack_ranges[1], &ack_ranges[0], sizeof(QuicAckRange) * (ack_range_count - 1));
        }
        ack_ranges[0].gap = gap;
        ack_ranges[0].range = range;

        // Out-of-order packet: force immediate ACK (mirrors nginx
        // ctx->send_ack = NGX_QUIC_MAX_ACK_GAP for pn > base with gap).
        if (ack_eliciting) {
            send_ack_count = kQuicMaxAckGap;
            send_ack = true;
        }
        return;
    }

    // -----------------------------------------------------------------------
    // pn < base: search existing ranges.
    // -----------------------------------------------------------------------
    // Check if PN is already covered by the first (largest) range.
    if (pn >= smallest && pn <= largest) {
        return;
    }

    // Walk the gap/range pairs.
    for (std::uint32_t i = 0; i < ack_range_count; ++i) {
        QuicAckRange &r = ack_ranges[i];

        // Gap bounds: (smallest - gap - 2) .. (smallest - 1)
        const std::uint64_t ge = smallest - 1;
        const std::uint64_t gs = ge - r.gap;

        if (pn >= gs && pn <= ge) {
            // PN falls inside this gap.
            if (gs == ge) {
                // Gap is exactly one packet — it's now filled.
                if (i == 0) {
                    first_range += r.range + 2;
                } else {
                    ack_ranges[i - 1].range += r.range + 2;
                }
                const std::uint32_t nr = ack_range_count - i - 1;
                if (nr > 0) {
                    std::memmove(&ack_ranges[i], &ack_ranges[i + 1], sizeof(QuicAckRange) * nr);
                }
                --ack_range_count;
            } else if (pn == gs) {
                // Gap shrinks from tail — current range grows.
                --r.gap;
                ++r.range;
            } else if (pn == ge) {
                // Gap shrinks from head — previous range grows.
                --r.gap;
                if (i == 0) {
                    ++first_range;
                } else {
                    ++ack_ranges[i - 1].range;
                }
            } else {
                // Gap is split into two parts.
                // If the ranges array is already full, generate a forced ACK
                // BEFORE the modification (same as the pn > base case).
                if (ack_range_count == kQuicMaxAckRanges && ack_eliciting && prev_pending_ack != kUnsetPacketNumber) {
                    generate_forced_ack(largest_range, first_range, ack_range_count, ack_ranges.data(), received_time);
                    if (prev_pending_ack == pending_ack) {
                        pending_ack = kUnsetPacketNumber;
                    }
                }

                record_ecn(ecn);

                const std::uint64_t new_gap = ge - pn - 1;

                r.gap = pn - gs - 1;

                // Insert a zero-range entry for the newly filled slot.
                const std::uint32_t insert_pos = i + 1;
                if (ack_range_count < kQuicMaxAckRanges) {
                    ++ack_range_count;
                }
                if (ack_range_count > insert_pos) {
                    std::memmove(&ack_ranges[insert_pos + 1], &ack_ranges[insert_pos],
                                 sizeof(QuicAckRange) * (ack_range_count - insert_pos - 1));
                }
                ack_ranges[insert_pos].gap = new_gap;
                ack_ranges[insert_pos].range = 0;
            }

            // pn < base: out-of-order, force immediate ACK (mirrors nginx
            // ctx->send_ack = NGX_QUIC_MAX_ACK_GAP for pn < base).
            if (ack_eliciting) {
                send_ack_count = kQuicMaxAckGap;
                send_ack = true;
            }
            if (pn == gs || pn == ge || gs == ge) {
                record_ecn(ecn);
            }
            return;
        }

        // Move to the next range.
        largest = smallest - r.gap - 2;
        smallest = largest - r.range;

        // Check if PN is already covered by this range.
        if (pn >= smallest && pn <= largest) {
            return;
        }
    }

    // PN is just below the last range — extend it.
    if (pn == smallest - 1) {
        record_ecn(ecn);
        if (ack_range_count == 0) {
            ++first_range;
        } else {
            ++ack_ranges[ack_range_count - 1].range;
        }
        return;
    }

    // PN is too old to keep — ranges array is full.
    // Mirror ngx_quic_send_ack_range: send a targeted ACK for just this
    // packet number if it is ack-eliciting, then return without tracking.
    if (ack_range_count == kQuicMaxAckRanges) {
        if (ack_eliciting) {
            generate_forced_ack(pn, 0, 0, nullptr, received_time);
            send_ack_count = kQuicMaxAckGap;
            send_ack = true;
        }
        return;
    }

    // Append a new gap+range entry at the tail.
    record_ecn(ecn);
    const std::uint64_t gap = smallest - 2 - pn;
    ack_ranges[ack_range_count].gap = gap;
    ack_ranges[ack_range_count].range = 0;
    ++ack_range_count;
}

// ---------------------------------------------------------------------------
// drop_ack_ranges — discard ACK ranges <= largest_acknowledged.
// Ported from nginx's ngx_quic_drop_ack_ranges (ngx_event_quic_ack.c).
// ---------------------------------------------------------------------------
void QuicPacketNumberSpace::drop_ack_ranges(std::uint64_t largest_acknowledged) noexcept {
    const std::uint64_t base = largest_range;

    if (base == kUnsetPacketNumber) {
        return;
    }

    // Clear pending_ack if it is covered by the drop.
    if (pending_ack != kUnsetPacketNumber && largest_acknowledged >= pending_ack) {
        pending_ack = kUnsetPacketNumber;
    }

    std::uint64_t largest = base;
    std::uint64_t smallest = largest - first_range;

    // If PN covers everything — reset all range state.
    if (largest_acknowledged >= largest) {
        largest_range = kUnsetPacketNumber;
        first_range = 0;
        ack_range_count = 0;
        return;
    }

    // If PN is inside the first range — truncate it.
    if (largest_acknowledged >= smallest) {
        first_range = largest - largest_acknowledged - 1;
        ack_range_count = 0;
        return;
    }

    // Walk subsequent gap/range pairs.
    for (std::uint32_t i = 0; i < ack_range_count; ++i) {
        QuicAckRange &r = ack_ranges[i];

        largest = smallest - r.gap - 2;
        smallest = largest - r.range;

        if (largest_acknowledged >= largest) {
            ack_range_count = i;
            return;
        }
        if (largest_acknowledged >= smallest) {
            r.range = largest - largest_acknowledged - 1;
            ack_range_count = i + 1;
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// quic_record_ack_eliciting_received
// Consolidates the inline ack-eliciting logic from QuicPacketProcessor.
// ---------------------------------------------------------------------------
void quic_record_ack_eliciting_received(QuicPacketNumberSpace &space, std::uint64_t packet_number,
                                        QuicTime received_time, std::uint64_t previous_largest_received) noexcept {
    if (!space.send_ack) {
        space.ack_delay_start = received_time;
    }
    ++space.send_ack_count;
    // pending_ack is now updated in on_packet_received() before range overflow
    // handling, matching nginx's ngx_quic_ack_packet where the pending_ack
    // update (lines 1206-1221) precedes the range checks.
    if (space.largest_received_packet_number == packet_number) {
        space.largest_received_time = received_time;
    }
    if (previous_largest_received != kUnsetPacketNumber && packet_number != previous_largest_received + 1) {
        space.send_ack_count = kQuicMaxAckGap;
    }
    space.send_ack = true;
}

} // namespace fiber::quic
