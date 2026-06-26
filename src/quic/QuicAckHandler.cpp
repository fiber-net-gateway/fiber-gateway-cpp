#include "QuicAckHandler.h"

#include <algorithm>
#include <array>
#include <expected>

#include "QuicCursor.h"
#include "QuicLossRecovery.h"
#include "QuicPath.h"
#include "QuicProtocol.h"
#include "QuicTransportCodec.h"

namespace fiber::quic {

namespace {

struct QuicAckedEcnPathStat {
    std::uint64_t path_seqnum = kQuicNoPathSeqnum;
    std::uint64_t ect0 = 0;
};

struct QuicAckedEcnStats {
    std::array<QuicAckedEcnPathStat, kQuicMaxPaths> paths{};
    std::uint64_t ect0 = 0;
    std::uint64_t ect1 = 0;
    QuicTime newest_ect_send_time{0};
    bool has_ect = false;
};

void add_ecn_path_stat(QuicAckedEcnStats &stats, std::uint64_t path_seqnum, std::uint64_t ect0) noexcept {
    if (path_seqnum == kQuicNoPathSeqnum || ect0 == 0) {
        return;
    }

    QuicAckedEcnPathStat *empty = nullptr;
    for (QuicAckedEcnPathStat &path: stats.paths) {
        if (path.path_seqnum == path_seqnum) {
            path.ect0 += ect0;
            return;
        }
        if (empty == nullptr && path.path_seqnum == kQuicNoPathSeqnum) {
            empty = &path;
        }
    }

    if (empty != nullptr) {
        empty->path_seqnum = path_seqnum;
        empty->ect0 = ect0;
    }
}

void record_acked_ecn_packet(const QuicOutputFrame &frame, QuicAckedEcnStats &stats) noexcept {
    switch (frame.packet_ecn) {
        case net::UdpEcn::Ect0:
            ++stats.ect0;
            add_ecn_path_stat(stats, frame.packet_path_seqnum, 1);
            break;
        case net::UdpEcn::Ect1:
            ++stats.ect1;
            break;
        case net::UdpEcn::Ce:
        case net::UdpEcn::NonEct:
        case net::UdpEcn::Unspecified:
            return;
    }

    stats.has_ect = true;
    if (stats.newest_ect_send_time < frame.send_time) {
        stats.newest_ect_send_time = frame.send_time;
    }
}

QuicPath *find_path_by_seqnum(QuicConnection &connection, std::uint64_t seqnum) noexcept {
    if (seqnum == kQuicNoPathSeqnum) {
        return nullptr;
    }
    for (QuicPath &path: connection.paths().paths()) {
        if (path.allocated && path.seqnum == seqnum) {
            return &path;
        }
    }
    return nullptr;
}

void fail_ecn_validation(QuicConnection &connection, const QuicAckedEcnStats &stats) noexcept {
    for (const QuicAckedEcnPathStat &path_stat: stats.paths) {
        QuicPath *path = find_path_by_seqnum(connection, path_stat.path_seqnum);
        if (path == nullptr || path_stat.ect0 == 0) {
            continue;
        }
        path->ecn_state = QuicEcnState::Failed;
    }
}

void confirm_ecn_validation(QuicConnection &connection, const QuicAckedEcnStats &stats) noexcept {
    for (const QuicAckedEcnPathStat &path_stat: stats.paths) {
        QuicPath *path = find_path_by_seqnum(connection, path_stat.path_seqnum);
        if (path == nullptr || path_stat.ect0 == 0 || path->ecn_state != QuicEcnState::Testing) {
            continue;
        }
        path->ecn_validation_acked += static_cast<std::uint32_t>(
                std::min<std::uint64_t>(path_stat.ect0, UINT32_MAX - path->ecn_validation_acked));
        path->ecn_state = QuicEcnState::Capable;
    }
}

[[nodiscard]] bool has_trusted_ecn_path(QuicConnection &connection, const QuicAckedEcnStats &stats) noexcept {
    for (const QuicAckedEcnPathStat &path_stat: stats.paths) {
        QuicPath *path = find_path_by_seqnum(connection, path_stat.path_seqnum);
        if (path != nullptr && path_stat.ect0 != 0 && path->ecn_state == QuicEcnState::Capable) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool reported_ecn_counters_valid(const QuicPacketNumberSpace &space,
                                               const QuicEcnCounters &reported) noexcept {
    if (reported.ect0 < space.peer_ecn_counters.ect0 || reported.ect1 < space.peer_ecn_counters.ect1 ||
        reported.ce < space.peer_ecn_counters.ce) {
        return false;
    }
    if (reported.ect1 > space.ecn_sent_counters.ect1) {
        return false;
    }
    if (reported.ect0 > space.ecn_sent_counters.ect0) {
        return false;
    }
    return reported.ce <= space.ecn_sent_counters.ect0 - reported.ect0;
}

void validate_ecn_feedback(QuicConnection &connection, QuicPacketNumberSpace &space, const QuicInputFrame &frame,
                           const QuicAckedEcnStats &stats, QuicTime now,
                           QuicAckProcessResult &result) noexcept {
    if (frame.type != QuicFrameType::AckEcn) {
        if (stats.has_ect) {
            fail_ecn_validation(connection, stats);
        }
        return;
    }

    QuicEcnCounters reported{};
    reported.ect0 = frame.u.ack.ect0;
    reported.ect1 = frame.u.ack.ect1;
    reported.ce = frame.u.ack.ce;
    if (!reported_ecn_counters_valid(space, reported)) {
        fail_ecn_validation(connection, stats);
        return;
    }

    const std::uint64_t delta_ect0 = reported.ect0 - space.peer_ecn_counters.ect0;
    const std::uint64_t delta_ect1 = reported.ect1 - space.peer_ecn_counters.ect1;
    const std::uint64_t delta_ce = reported.ce - space.peer_ecn_counters.ce;
    const bool counters_cover_acked_ect = delta_ect0 + delta_ce >= stats.ect0 && delta_ect1 >= stats.ect1;
    if (stats.has_ect && !counters_cover_acked_ect) {
        fail_ecn_validation(connection, stats);
        return;
    }

    space.peer_ecn_counters = reported;
    if (!stats.has_ect) {
        return;
    }

    confirm_ecn_validation(connection, stats);
    if (delta_ce == 0 || !has_trusted_ecn_path(connection, stats)) {
        return;
    }

    std::size_t path_mtu = connection.congestion().mtu;
    for (const QuicAckedEcnPathStat &path_stat: stats.paths) {
        if (QuicPath *path = find_path_by_seqnum(connection, path_stat.path_seqnum)) {
            path_mtu = path->mtu;
            break;
        }
    }
    result.unblocked = result.unblocked ||
                       quic_congestion_on_ecn_ce(connection.congestion(), stats.newest_ect_send_time, now, path_mtu);
}

[[nodiscard]] common::IoResult<void> handle_ack_range(QuicConnection &connection, QuicPacketNumberSpace &space,
                                                      std::uint64_t min_packet_number, std::uint64_t max_packet_number,
                                                      QuicTime now, QuicLossAckStat &stat,
                                                      QuicAckProcessResult &result,
                                                      QuicAckedEcnStats &ecn_stats) noexcept {
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
                record_acked_ecn_packet(*frame, ecn_stats);
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
        if (space.level == QuicEncryptionLevel::Application) {
            auto mtu = connection.paths().handle_mtu_ack(min_packet_number, max_packet_number, now);
            if (!mtu) {
                return std::unexpected(mtu.error());
            }
        }
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
    QuicAckedEcnStats ecn_stats{};
    auto handled = handle_ack_range(connection, space, min_packet_number, max_packet_number, now, stat, result,
                                    ecn_stats);
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
        handled = handle_ack_range(connection, space, min_packet_number, max_packet_number, now, stat, result,
                                   ecn_stats);
        if (!handled) {
            return std::unexpected(handled.error());
        }
    }

    validate_ecn_feedback(connection, space, frame, ecn_stats, now, result);

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
