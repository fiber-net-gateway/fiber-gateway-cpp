#include "QuicLossRecovery.h"

#include <expected>
#include <limits>

#include "QuicProtocol.h"

namespace fiber::quic {

namespace {

inline constexpr QuicEncryptionLevel kLossLevels[] = {QuicEncryptionLevel::Initial, QuicEncryptionLevel::Handshake,
                                                      QuicEncryptionLevel::Application};

[[nodiscard]] QuicTime max_time() noexcept { return QuicTime{std::numeric_limits<std::int64_t>::max()}; }

[[nodiscard]] QuicTime pto_duration(const QuicConnection &connection, QuicEncryptionLevel level) noexcept {
    return quic_pto(connection.rtt(), connection.peer_transport().params.max_ack_delay,
                    level == QuicEncryptionLevel::Application, connection.state() == QuicConnectionState::Established);
}

[[nodiscard]] QuicTime pto_backoff(QuicTime base, std::uint32_t count) noexcept {
    if (base <= QuicTime{0}) {
        return QuicTime{0};
    }

    std::int64_t value = base.count();
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    while (count-- != 0) {
        if (value > kMax / 2) {
            return QuicTime{kMax};
        }
        value *= 2;
    }
    return QuicTime{value};
}

[[nodiscard]] bool should_retransmit_lost_frame(const QuicConnection &connection,
                                                const QuicOutputFrame &frame) noexcept {
    switch (frame.type) {
        case QuicFrameType::DataBlocked:
            return connection.should_retransmit_data_blocked(frame.u.data_blocked.limit);
        case QuicFrameType::StreamDataBlocked:
            return connection.should_retransmit_stream_data_blocked(frame.u.stream_data_blocked.id,
                                                                    frame.u.stream_data_blocked.limit);
        case QuicFrameType::MaxStreamsBidi:
            return connection.should_retransmit_max_streams(QuicStreamType::Bidirectional, frame.u.max_streams.limit);
        case QuicFrameType::MaxStreamsUni:
            return connection.should_retransmit_max_streams(QuicStreamType::Unidirectional, frame.u.max_streams.limit);
        case QuicFrameType::StreamsBlockedBidi:
            return connection.should_retransmit_streams_blocked(QuicStreamType::Bidirectional,
                                                                frame.u.streams_blocked.limit);
        case QuicFrameType::StreamsBlockedUni:
            return connection.should_retransmit_streams_blocked(QuicStreamType::Unidirectional,
                                                                frame.u.streams_blocked.limit);
        case QuicFrameType::NewConnectionId:
            return connection.should_retransmit_new_connection_id(frame.u.new_connection_id.sequence_number);
        case QuicFrameType::RetireConnectionId:
            return connection.should_retransmit_retire_connection_id(frame.u.retire_connection_id.sequence_number);
        default:
            return quic_output_frame_retransmittable_on_loss(frame.type);
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

void record_ecn_probe_lost(QuicConnection &connection, const QuicOutputFrame &frame) noexcept {
    if (!frame.packet_ecn_validation_probe || frame.packet_len == 0) {
        return;
    }

    QuicPath *path = find_path_by_seqnum(connection, frame.packet_path_seqnum);
    if (path == nullptr || path->ecn_state != QuicEcnState::Testing) {
        return;
    }

    if (path->ecn_validation_lost != UINT32_MAX) {
        ++path->ecn_validation_lost;
    }
    if (path->ecn_validation_acked == 0 && path->ecn_validation_sent != 0 &&
        path->ecn_validation_lost >= path->ecn_validation_sent) {
        path->ecn_state = QuicEcnState::Failed;
    }
}

} // namespace

QuicTime quic_oldest_sent_time(QuicConnection &connection) noexcept {
    QuicTime oldest = QuicTime::max();
    for (QuicEncryptionLevel level: kLossLevels) {
        QuicOutputFrame *frame = connection.packet_number_space(level).sent_frames.front();
        if (frame != nullptr && frame->send_time < oldest) {
            oldest = frame->send_time;
        }
    }
    return oldest == QuicTime::max() ? QuicTime{0} : oldest;
}

common::IoResult<QuicLossRecoveryResult> quic_detect_lost(QuicConnection &connection, QuicTime now,
                                                          const QuicLossAckStat *stat) noexcept {
    QuicLossRecoveryResult result{};
    const QuicTime threshold = quic_loss_time_threshold(connection.rtt());
    std::uint32_t lost_count = 0;
    QuicTime oldest_lost{0};
    QuicTime newest_lost{0};

    for (QuicEncryptionLevel level: kLossLevels) {
        QuicPacketNumberSpace &space = connection.packet_number_space(level);
        if (space.largest_acked_packet_number == kUnsetPacketNumber) {
            continue;
        }

        QuicOutputFrame *front = space.sent_frames.front();
        if (front == nullptr) {
            continue;
        }
        const std::uint64_t packet_threshold =
                quic_loss_packet_threshold(space.next_packet_number, front->packet_number);

        while ((front = space.sent_frames.front()) != nullptr) {
            if (front->packet_number > space.largest_acked_packet_number) {
                break;
            }

            const bool time_lost = front->send_time + threshold <= now;
            const bool packet_lost = space.largest_acked_packet_number - front->packet_number >= packet_threshold;
            if (!time_lost && !packet_lost) {
                break;
            }

            if (front->send_time > connection.rtt().first_rtt) {
                if (lost_count == 0 || front->send_time < oldest_lost) {
                    oldest_lost = front->send_time;
                }
                if (lost_count == 0 || front->send_time > newest_lost) {
                    newest_lost = front->send_time;
                }
                ++lost_count;
            }

            if (front->packet_len != 0) {
                record_ecn_probe_lost(connection, *front);
                const bool unblocked = quic_congestion_on_loss(
                        connection.congestion(),
                        QuicLossSample{front->packet_len, front->packet_number, front->send_time, front->ignore_loss},
                        connection.reset_packet_number(), now, connection.congestion().mtu);
                result.unblocked = result.unblocked || unblocked;
            }

            (void) space.sent_frames.pop_front();

            if ((front->type == QuicFrameType::Ack || front->type == QuicFrameType::AckEcn) &&
                level == QuicEncryptionLevel::Application) {
                space.send_ack_count = kQuicMaxAckGap;
                space.send_ack = true;
                result.force_send = true;
            }

            front->packet_number = 0;
            front->packet_len = 0;
            front->send_time = QuicTime{0};
            front->packet_ack_eliciting = false;
            if (front->type == QuicFrameType::Stream) {
                auto failed = connection.on_stream_send_failed(front->u.stream.stream_id,
                                                               static_cast<std::size_t>(front->u.stream.offset),
                                                               front->u.stream.length, front->u.stream.fin);
                if (!failed) {
                    return std::unexpected(failed.error());
                }
                space.release_frame(*front);
                result.lost_frames = true;
                continue;
            }
            if (should_retransmit_lost_frame(connection, *front)) {
                space.pending_frames.push_back(*front);
                result.lost_frames = true;
            } else {
                space.release_frame(*front);
            }
        }
    }

    if (stat != nullptr && lost_count >= 2 && stat->oldest != QuicTime::max() &&
        (stat->newest < oldest_lost || stat->oldest > newest_lost) &&
        newest_lost - oldest_lost > quic_persistent_congestion_duration(
                                            connection.rtt(), connection.peer_transport().params.max_ack_delay)) {
        quic_congestion_on_persistent_congestion(connection.congestion(), quic_oldest_sent_time(connection),
                                                 connection.congestion().mtu);
    }

    return result;
}

QuicLossDetectionTimer quic_loss_detection_timer(const QuicConnection &connection, QuicTime now,
                                                 std::uint32_t pto_count) noexcept {
    QuicTime lost_delay = max_time();
    QuicTime pto_delay = max_time();
    bool has_lost = false;
    bool has_pto = false;

    for (QuicEncryptionLevel level: kLossLevels) {
        const QuicPacketNumberSpace &space = connection.packet_number_space(level);
        if (space.sent_frames.empty()) {
            continue;
        }

        if (space.largest_acked_packet_number != kUnsetPacketNumber) {
            const QuicOutputFrame *front = space.sent_frames.front();
            QuicTime wait = front->send_time + quic_loss_time_threshold(connection.rtt()) - now;

            if (front->packet_number <= space.largest_acked_packet_number) {
                const std::uint64_t packet_threshold =
                        quic_loss_packet_threshold(space.next_packet_number, front->packet_number);

                if (wait < QuicTime{0} ||
                    space.largest_acked_packet_number - front->packet_number >= packet_threshold) {
                    wait = QuicTime{0};
                }

                if (!has_lost || wait < lost_delay) {
                    lost_delay = wait;
                    has_lost = true;
                }
            }
        }

        const QuicOutputFrame *back = space.sent_frames.back();
        QuicTime wait = back->send_time + pto_backoff(pto_duration(connection, level), pto_count) - now;
        if (wait < QuicTime{0}) {
            wait = QuicTime{0};
        }
        if (!has_pto || wait < pto_delay) {
            pto_delay = wait;
            has_pto = true;
        }
    }

    if (has_lost) {
        return {.mode = QuicLossTimerMode::Lost, .delay = lost_delay};
    }
    if (has_pto) {
        return {.mode = QuicLossTimerMode::Pto, .delay = pto_delay};
    }
    return {};
}

common::IoResult<bool> quic_queue_pto_probe_frames(QuicConnection &connection, QuicTime now,
                                                   std::uint32_t pto_count) noexcept {
    bool queued = false;

    for (QuicEncryptionLevel level: kLossLevels) {
        QuicPacketNumberSpace &space = connection.packet_number_space(level);
        if (space.sent_frames.empty()) {
            continue;
        }

        const QuicOutputFrame *back = space.sent_frames.back();
        QuicTime wait = back->send_time + pto_backoff(pto_duration(connection, level), pto_count) - now;

        if (back->packet_number <= space.largest_acked_packet_number &&
            space.largest_acked_packet_number != kUnsetPacketNumber) {
            continue;
        }

        if (wait > QuicTime{0}) {
            continue;
        }

        for (std::uint8_t i = 0; i < 2; ++i) {
            QuicOutputFrame *frame = space.alloc_frame();
            if (frame == nullptr) {
                return std::unexpected(common::IoErr::NoMem);
            }

            frame->type = QuicFrameType::Ping;
            frame->ignore_congestion = true;
            frame->pto_probe = true;
            space.pending_frames.push_front(*frame);
        }

        queued = true;
    }

    return queued;
}

} // namespace fiber::quic
