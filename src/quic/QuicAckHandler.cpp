#include "QuicAckHandler.h"

#include <expected>

#include "QuicCursor.h"
#include "QuicLossRecovery.h"
#include "QuicProtocol.h"
#include "QuicTransportCodec.h"

namespace fiber::quic {

namespace {

[[nodiscard]] common::IoResult<void> handle_ack_range(QuicConnection &connection, QuicPacketNumberSpace &space,
                                                      std::uint64_t min_packet_number, std::uint64_t max_packet_number,
                                                      QuicTime now, QuicLossAckStat &stat,
                                                      QuicAckProcessResult &result) noexcept {
    bool found = false;
    QuicOutputFrame *prev = nullptr;
    QuicOutputFrame *frame = space.sent_frames.front();
    while (frame != nullptr) {
        QuicOutputFrame *next = space.sent_frames.next_of(*frame);
        if (frame->packet_number > max_packet_number) {
            break;
        }

        if (frame->packet_number >= min_packet_number) {
            if (frame->packet_len != 0) {
                const bool unblocked = quic_congestion_on_ack(
                        connection.congestion(),
                        QuicAckSample{frame->packet_len, frame->packet_number, frame->send_time},
                        connection.reset_packet_number(), now, quic_oldest_sent_time(connection));
                result.unblocked = result.unblocked || unblocked;
            }

            if (frame->packet_number == max_packet_number && frame->packet_ack_eliciting) {
                stat.max_packet_send_time = frame->send_time;
                stat.max_packet_ack_eliciting = true;
            }
            if (stat.oldest == QuicTime::max() || frame->send_time < stat.oldest) {
                stat.oldest = frame->send_time;
            }
            if (stat.newest == QuicTime::max() || frame->send_time > stat.newest) {
                stat.newest = frame->send_time;
            }

            space.sent_frames.erase_after(prev, *frame);
            frame->packet_len = 0;
            frame->packet_ack_eliciting = false;

            // RFC 9000, 13.2.4: Limiting Ranges by Tracking ACK Frames.
            // When an ACK frame we sent is acknowledged, drop ranges up to
            // that point to prevent generating ACKs for already-ACKed data.
            if (frame->type == QuicFrameType::Ack || frame->type == QuicFrameType::AckEcn) {
                space.drop_ack_ranges(frame->packet_number);
            }

            if (frame->type == QuicFrameType::Stream) {
                auto acked = connection.on_stream_send_acked(frame->u.stream.stream_id,
                                                             static_cast<std::size_t>(frame->u.stream.offset),
                                                             frame->u.stream.length, frame->u.stream.fin);
                if (!acked) {
                    return std::unexpected(acked.error());
                }
            }

            space.release_frame(*frame);
            found = true;
            result.acked_frames = true;
        } else {
            prev = frame;
        }

        frame = next;
    }

    if (!found && max_packet_number >= space.next_packet_number) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (found) {
        connection.reset_pto_count();
    }
    return {};
}

} // namespace

common::IoResult<QuicAckProcessResult> quic_handle_ack_frame(QuicConnection &connection, QuicEncryptionLevel level,
                                                             const QuicInputFrame &frame, QuicTime now,
                                                             std::uint64_t ack_delay_exponent, QuicTime max_ack_delay,
                                                             bool handshake_confirmed) noexcept {
    if (frame.type != QuicFrameType::Ack && frame.type != QuicFrameType::AckEcn) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (frame.u.ack.first_range > frame.u.ack.largest) {
        return std::unexpected(common::IoErr::Invalid);
    }

    QuicPacketNumberSpace &space = connection.packet_number_space(level);
    QuicAckProcessResult result{};
    QuicLossAckStat stat{};

    std::uint64_t min_packet_number = frame.u.ack.largest - frame.u.ack.first_range;
    std::uint64_t max_packet_number = frame.u.ack.largest;
    auto handled = handle_ack_range(connection, space, min_packet_number, max_packet_number, now, stat, result);
    if (!handled) {
        return std::unexpected(handled.error());
    }

    const bool largest_newly_acked = space.largest_acked_packet_number == kUnsetPacketNumber ||
                                     max_packet_number > space.largest_acked_packet_number;
    if (largest_newly_acked) {
        space.record_acked_packet_number(max_packet_number);
        if (stat.max_packet_ack_eliciting && stat.max_packet_send_time != QuicTime::max()) {
            quic_rtt_sample(connection.rtt(), now, stat.max_packet_send_time, frame.u.ack.delay, ack_delay_exponent,
                            max_ack_delay, handshake_confirmed);
        }
    }

    QuicReadCursor ranges(frame.data.data, frame.data.len);
    for (std::uint64_t i = 0; i < frame.u.ack.range_count; ++i) {
        auto gap = quic_parse_varint(ranges);
        auto range = gap ? quic_parse_varint(ranges) : std::unexpected(gap.error());
        if (!range) {
            return std::unexpected(range.error());
        }
        if (*gap + 2 > min_packet_number) {
            return std::unexpected(common::IoErr::Invalid);
        }
        max_packet_number = min_packet_number - *gap - 2;
        if (*range > max_packet_number) {
            return std::unexpected(common::IoErr::Invalid);
        }
        min_packet_number = max_packet_number - *range;
        handled = handle_ack_range(connection, space, min_packet_number, max_packet_number, now, stat, result);
        if (!handled) {
            return std::unexpected(handled.error());
        }
    }

    auto lost = quic_detect_lost(connection, now, &stat);
    if (!lost) {
        return std::unexpected(lost.error());
    }
    result.unblocked = result.unblocked || lost->unblocked;
    result.lost_frames = result.lost_frames || lost->lost_frames;
    result.force_send = result.force_send || lost->force_send;
    return result;
}

} // namespace fiber::quic
