#include "QuicAckHandler.h"

#include <expected>

#include "QuicCursor.h"
#include "QuicProtocol.h"
#include "QuicTransportCodec.h"

namespace fiber::quic {

namespace {

struct AckStat {
    QuicTime max_packet_send_time{QuicTime::max()};
    QuicTime oldest{QuicTime::max()};
    QuicTime newest{QuicTime::max()};
    bool max_packet_ack_eliciting = false;
};

[[nodiscard]] bool retransmittable_on_loss(QuicFrameType type) noexcept {
    switch (type) {
        case QuicFrameType::Ack:
        case QuicFrameType::AckEcn:
        case QuicFrameType::Padding:
        case QuicFrameType::Ping:
        case QuicFrameType::PathChallenge:
        case QuicFrameType::PathResponse:
        case QuicFrameType::ConnectionClose:
        case QuicFrameType::ConnectionCloseApp:
            return false;

        case QuicFrameType::Crypto:
        case QuicFrameType::ResetStream:
        case QuicFrameType::StopSending:
        case QuicFrameType::NewToken:
        case QuicFrameType::Stream:
        case QuicFrameType::Stream1:
        case QuicFrameType::Stream2:
        case QuicFrameType::Stream3:
        case QuicFrameType::Stream4:
        case QuicFrameType::Stream5:
        case QuicFrameType::Stream6:
        case QuicFrameType::Stream7:
        case QuicFrameType::MaxData:
        case QuicFrameType::MaxStreamData:
        case QuicFrameType::MaxStreamsBidi:
        case QuicFrameType::MaxStreamsUni:
        case QuicFrameType::DataBlocked:
        case QuicFrameType::StreamDataBlocked:
        case QuicFrameType::StreamsBlockedBidi:
        case QuicFrameType::StreamsBlockedUni:
        case QuicFrameType::NewConnectionId:
        case QuicFrameType::RetireConnectionId:
        case QuicFrameType::HandshakeDone:
            return true;
    }
    return false;
}

[[nodiscard]] QuicTime oldest_sent_time(QuicConnection &connection) noexcept {
    QuicTime oldest = QuicTime::max();
    constexpr QuicEncryptionLevel levels[] = {QuicEncryptionLevel::Initial, QuicEncryptionLevel::Handshake,
                                              QuicEncryptionLevel::Application};
    for (QuicEncryptionLevel level: levels) {
        QuicFrame *frame = connection.packet_number_space(level).sent_frames.front();
        if (frame != nullptr && frame->send_time < oldest) {
            oldest = frame->send_time;
        }
    }
    return oldest == QuicTime::max() ? QuicTime{0} : oldest;
}

[[nodiscard]] common::IoResult<void> handle_ack_range(QuicConnection &connection, QuicPacketNumberSpace &space,
                                                      std::uint64_t min_packet_number, std::uint64_t max_packet_number,
                                                      QuicTime now, AckStat &stat,
                                                      QuicAckProcessResult &result) noexcept {
    bool found = false;
    QuicFrame *frame = space.sent_frames.front();
    while (frame != nullptr) {
        QuicFrame *next = space.sent_frames.next_of(*frame);
        if (frame->packet_number > max_packet_number) {
            break;
        }

        if (frame->packet_number >= min_packet_number) {
            if (frame->packet_len != 0) {
                const bool unblocked =
                        quic_congestion_on_ack(connection.congestion(),
                                               QuicAckSample{frame->packet_len, frame->packet_number, frame->send_time},
                                               connection.reset_packet_number(), now, oldest_sent_time(connection));
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

            space.sent_frames.erase(*frame);
            frame->packet_len = 0;
            frame->packet_ack_eliciting = false;
            found = true;
            result.acked_frames = true;
        }

        frame = next;
    }

    if (!found && max_packet_number >= space.next_packet_number) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return {};
}

[[nodiscard]] common::IoResult<void> detect_lost(QuicConnection &connection, QuicTime now, const AckStat &stat,
                                                 QuicAckProcessResult &result) noexcept {
    const QuicTime threshold = quic_loss_time_threshold(connection.rtt());
    std::uint32_t lost_count = 0;
    QuicTime oldest_lost{0};
    QuicTime newest_lost{0};

    constexpr QuicEncryptionLevel levels[] = {QuicEncryptionLevel::Initial, QuicEncryptionLevel::Handshake,
                                              QuicEncryptionLevel::Application};
    for (QuicEncryptionLevel level: levels) {
        QuicPacketNumberSpace &space = connection.packet_number_space(level);
        if (space.largest_acked_packet_number == kUnsetPacketNumber) {
            continue;
        }

        QuicFrame *front = space.sent_frames.front();
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
                const bool unblocked = quic_congestion_on_loss(
                        connection.congestion(),
                        QuicLossSample{front->packet_len, front->packet_number, front->send_time, front->ignore_loss},
                        connection.reset_packet_number(), now, connection.congestion().mtu);
                result.unblocked = result.unblocked || unblocked;
            }

            space.sent_frames.erase(*front);
            front->packet_len = 0;
            front->packet_ack_eliciting = false;
            if (retransmittable_on_loss(front->type)) {
                space.pending_frames.push_back(*front);
                result.lost_frames = true;
            }
        }
    }

    if (lost_count >= 2 && stat.oldest != QuicTime::max() && (stat.newest < oldest_lost || stat.oldest > newest_lost) &&
        newest_lost - oldest_lost > quic_persistent_congestion_duration(connection.rtt(), QuicTime{25})) {
        quic_congestion_on_persistent_congestion(connection.congestion(), oldest_sent_time(connection),
                                                 connection.congestion().mtu);
    }

    return {};
}

} // namespace

common::IoResult<QuicAckProcessResult> quic_handle_ack_frame(QuicConnection &connection, QuicEncryptionLevel level,
                                                             const QuicFrame &frame, QuicTime now,
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
    AckStat stat{};

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

    auto lost = detect_lost(connection, now, stat, result);
    if (!lost) {
        return std::unexpected(lost.error());
    }
    return result;
}

} // namespace fiber::quic
