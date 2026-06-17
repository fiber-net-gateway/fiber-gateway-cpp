#include "QuicPacketProcessor.h"

#include <algorithm>
#include <cstring>
#include <expected>
#include <memory>

#include "QuicAckHandler.h"
#include "QuicCrypto.h"
#include "QuicCursor.h"
#include "QuicPacketCodec.h"
#include "QuicPacketNumberSpace.h"
#include "QuicTransportCodec.h"

namespace fiber::quic {

namespace {

[[nodiscard]] bool ip_address_equal(const net::IpAddress &lhs, const net::IpAddress &rhs) noexcept {
    if (lhs.family() != rhs.family()) {
        return false;
    }
    if (lhs.is_v4()) {
        return lhs.v4_bytes() == rhs.v4_bytes();
    }
    return lhs.scope_id() == rhs.scope_id() && lhs.v6_bytes() == rhs.v6_bytes();
}

[[nodiscard]] bool socket_address_equal(const net::SocketAddress &lhs, const net::SocketAddress &rhs) noexcept {
    return lhs.port() == rhs.port() && ip_address_equal(lhs.ip(), rhs.ip());
}

[[nodiscard]] bool connection_id_equal(const QuicConnectionId &lhs, const QuicConnectionId &rhs) noexcept {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    if (lhs.size() == 0) {
        return true;
    }
    return std::equal(lhs.data(), lhs.data() + lhs.size(), rhs.data());
}

[[nodiscard]] bool probing_frame(QuicFrameType type) noexcept {
    return type == QuicFrameType::Padding || type == QuicFrameType::PathChallenge ||
           type == QuicFrameType::PathResponse || type == QuicFrameType::NewConnectionId;
}

[[nodiscard]] common::IoResult<QuicPath *> bind_datagram_path(QuicConnection &conn,
                                                              const QuicReceivedDatagram &datagram,
                                                              const QuicPacketHeader &packet,
                                                              std::size_t received_len) noexcept {
    if (QuicPath *path = conn.find_path(datagram.peer, datagram.local)) {
        conn.record_path_received(*path, received_len);
        return path;
    }

    auto &space = conn.packet_number_space(packet.level);
    if (packet.packet_number != space.largest_received_packet_number) {
        return std::unexpected(common::IoErr::Invalid);
    }

    if (QuicPath *probe = conn.find_path(QuicPathTag::Probe)) {
        conn.free_path(*probe);
    }

    QuicPath *path = conn.create_path(datagram.peer, datagram.local, packet.scid, QuicPathTag::Probe);
    if (path == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }
    conn.record_path_received(*path, received_len);
    return path;
}

void discard_frame_queue(QuicPacketNumberSpace &space, QuicOutputFrameQueue &queue) noexcept {
    while (QuicOutputFrame *frame = queue.pop_front()) {
        space.release_frame(*frame);
    }
}

void discard_packet_number_space(QuicConnection &conn, QuicEncryptionLevel level) noexcept {
    QuicPacketNumberSpace &space = conn.packet_number_space(level);
    discard_frame_queue(space, space.pending_frames);
    discard_frame_queue(space, space.sending_frames);
    discard_frame_queue(space, space.sent_frames);
    if (space.ack_frame.queued) {
        space.pending_frames.erase(space.ack_frame);
    }
    space.send_ack = false;
    space.send_ack_count = 0;
    space.pending_ack = kUnsetPacketNumber;

    if (QuicPacketProtectionKeys *read = quic_packet_keys(conn.crypto(), level, false)) {
        read->reset();
    }
    if (QuicPacketProtectionKeys *write = quic_packet_keys(conn.crypto(), level, true)) {
        write->reset();
    }
}

[[nodiscard]] common::IoResult<void> queue_handshake_done(QuicConnection &conn) noexcept {
    if (conn.role() != QuicConnectionRole::Server) {
        return {};
    }

    QuicPacketNumberSpace &space = conn.packet_number_space(QuicEncryptionLevel::Application);
    QuicOutputFrame *frame = space.alloc_frame();
    if (frame == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }
    frame->type = QuicFrameType::HandshakeDone;
    space.pending_frames.push_back(*frame);
    return {};
}

[[nodiscard]] common::IoResult<void> buffer_crypto_segment(QuicCryptoRecvBuffer &buffer, std::uint64_t offset,
                                                           QuicSlice data) noexcept {
    if (data.len == 0) {
        return {};
    }
    const std::uint64_t end = offset + data.len;
    if (end <= buffer.next_offset) {
        return {};
    }
    if (offset < buffer.next_offset) {
        const std::size_t skip = static_cast<std::size_t>(buffer.next_offset - offset);
        offset = buffer.next_offset;
        data.data += skip;
        data.len -= skip;
    }
    if (offset + data.len > buffer.next_offset + kQuicMaxCryptoBuffered) {
        return std::unexpected(common::IoErr::MessageTooLarge);
    }

    for (QuicCryptoBufferedSegment &segment: buffer.segments) {
        if (segment.used && segment.offset == offset) {
            return {};
        }
    }

    for (QuicCryptoBufferedSegment &segment: buffer.segments) {
        if (!segment.used) {
            segment.data = std::make_unique<std::uint8_t[]>(data.len);
            if (!segment.data) {
                return std::unexpected(common::IoErr::NoMem);
            }
            std::memcpy(segment.data.get(), data.data, data.len);
            segment.offset = offset;
            segment.len = data.len;
            segment.used = true;
            return {};
        }
    }

    return std::unexpected(common::IoErr::NoMem);
}

[[nodiscard]] common::IoResult<void> provide_crypto_data(QuicConnection &conn, QuicEncryptionLevel level,
                                                         std::uint64_t offset, QuicSlice data) noexcept {
    if (!conn.tls().initialized()) {
        return {};
    }

    QuicCryptoRecvBuffer &buffer = conn.crypto_recv_buffer(level);
    if (offset > buffer.next_offset) {
        return buffer_crypto_segment(buffer, offset, data);
    }

    if (offset < buffer.next_offset) {
        const std::uint64_t skip64 = buffer.next_offset - offset;
        if (skip64 >= data.len) {
            return {};
        }
        const std::size_t skip = static_cast<std::size_t>(skip64);
        data.data += skip;
        data.len -= skip;
    }

    if (data.len != 0) {
        auto provided = conn.tls().provide_crypto_data(level, data.data, data.len);
        if (!provided) {
            return std::unexpected(provided.error());
        }
        buffer.next_offset += data.len;
    }

    bool progressed = true;
    while (progressed) {
        progressed = false;
        for (QuicCryptoBufferedSegment &segment: buffer.segments) {
            if (!segment.used || segment.offset != buffer.next_offset) {
                continue;
            }
            auto provided = conn.tls().provide_crypto_data(level, segment.data.get(), segment.len);
            if (!provided) {
                return std::unexpected(provided.error());
            }
            buffer.next_offset += segment.len;
            segment.data.reset();
            segment.offset = 0;
            segment.len = 0;
            segment.used = false;
            progressed = true;
            break;
        }
    }

    return {};
}

[[nodiscard]] common::IoResult<bool> handle_crypto_frame(QuicConnection &conn, QuicEncryptionLevel level,
                                                         const QuicInputFrame &frame) noexcept {
    auto provided = provide_crypto_data(conn, level, frame.u.crypto.offset, frame.data);
    if (!provided) {
        return std::unexpected(provided.error());
    }

    if (!conn.tls().initialized()) {
        return false;
    }

    auto driven = conn.tls().drive_handshake();
    if (!driven && driven.error() != common::IoErr::WouldBlock) {
        return std::unexpected(driven.error());
    }

    if (conn.tls().handshake_done() && conn.state() != QuicConnectionState::Established) {
        auto established = conn.mark_established();
        if (!established) {
            return std::unexpected(established.error());
        }
        auto queued = queue_handshake_done(conn);
        if (!queued) {
            return std::unexpected(queued.error());
        }
        discard_packet_number_space(conn, QuicEncryptionLevel::Handshake);
        return true;
    }

    return !conn.packet_number_space(level).pending_frames.empty() ||
           !conn.packet_number_space(QuicEncryptionLevel::Handshake).pending_frames.empty() ||
           !conn.packet_number_space(QuicEncryptionLevel::Application).pending_frames.empty();
}

[[nodiscard]] common::IoResult<void> handle_ack_eliciting_packet(QuicPacketNumberSpace &space,
                                                                 const QuicPacketHeader &packet,
                                                                 const QuicReceivedDatagram &datagram,
                                                                 std::uint64_t previous_largest_received,
                                                                 bool ack_eliciting) noexcept {
    if (!ack_eliciting) {
        return {};
    }

    const QuicTime now = quic_time_ms(datagram.received_at);
    quic_record_ack_eliciting_received(space, packet.packet_number, now, previous_largest_received);
    return {};
}

[[nodiscard]] common::IoResult<QuicPacketProcessResult>
process_decoded_packet(QuicConnection &conn, const QuicReceivedDatagram &datagram,
                       const QuicPacketDecodeResult &decoded, std::size_t received_len,
                       std::uint64_t previous_largest_received) noexcept {
    const QuicPacketHeader &packet = decoded.header;
    QuicPacketNumberSpace &space = conn.packet_number_space(packet.level);

    auto bound_path = bind_datagram_path(conn, datagram, packet, received_len);
    if (!bound_path) {
        return std::unexpected(bound_path.error());
    }

    QuicPacketProcessResult result{};
    result.path = *bound_path;
    result.created_path = result.path->tag == QuicPathTag::Probe;
    result.rebound =
            result.created_path && (connection_id_equal(packet.dcid, conn.local_connection_id()) ||
                                    connection_id_equal(packet.dcid, conn.original_destination_connection_id()));
    result.packet_type = packet.type;
    result.level = packet.level;
    result.packet_number = packet.packet_number;
    result.packet_count = 1;

    QuicReadCursor payload(decoded.payload.data, decoded.payload.len);
    while (!payload.empty()) {
        auto parsed = quic_parse_frame_for_receiver(conn.role(), packet.level, payload);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }

        QuicInputFrame &frame = parsed->frame;
        ++result.frame_count;
        result.ack_eliciting = result.ack_eliciting || frame.ack_eliciting;
        result.non_probing = result.non_probing || !probing_frame(frame.type);

        switch (frame.type) {
            case QuicFrameType::Ack:
            case QuicFrameType::AckEcn: {
                const QuicPeerTransportState &peer = conn.peer_transport();
                auto acked = quic_handle_ack_frame(conn, packet.level, frame, quic_time_ms(datagram.received_at),
                                                   peer.params.ack_delay_exponent, peer.params.max_ack_delay,
                                                   conn.state() == QuicConnectionState::Established);
                if (!acked) {
                    return std::unexpected(acked.error());
                }
                result.send_output = result.send_output || acked->unblocked || acked->lost_frames || acked->force_send;
                break;
            }
            case QuicFrameType::Padding:
            case QuicFrameType::Ping:
                break;
            case QuicFrameType::ConnectionClose:
            case QuicFrameType::ConnectionCloseApp:
                conn.begin_draining(static_cast<QuicErrorCode>(frame.u.close.error_code));
                break;
            case QuicFrameType::Crypto: {
                auto handled = handle_crypto_frame(conn, packet.level, frame);
                if (!handled) {
                    return std::unexpected(handled.error());
                }
                result.send_output = result.send_output || *handled;
                break;
            }
            case QuicFrameType::Stream: {
                auto received = conn.recv_stream_frame(frame.u.stream, frame.data);
                if (!received) {
                    return std::unexpected(received.error());
                }
                break;
            }
            case QuicFrameType::ResetStream: {
                auto reset = conn.recv_reset_stream_frame(frame.u.reset_stream);
                if (!reset) {
                    return std::unexpected(reset.error());
                }
                break;
            }
            case QuicFrameType::StopSending: {
                auto stopped = conn.recv_stop_sending_frame(frame.u.stop_sending);
                if (!stopped) {
                    return std::unexpected(stopped.error());
                }
                break;
            }
            case QuicFrameType::MaxStreamData: {
                auto updated = conn.recv_max_stream_data_frame(frame.u.max_stream_data);
                if (!updated) {
                    return std::unexpected(updated.error());
                }
                break;
            }
            case QuicFrameType::MaxData: {
                auto updated = conn.recv_max_data_frame(frame.u.max_data);
                if (!updated) {
                    return std::unexpected(updated.error());
                }
                break;
            }
            case QuicFrameType::StreamDataBlocked:
                if (conn.find_stream(frame.u.stream_data_blocked.id) == nullptr) {
                    return std::unexpected(common::IoErr::Invalid);
                }
                break;
            case QuicFrameType::MaxStreamsBidi:
            case QuicFrameType::MaxStreamsUni:
            case QuicFrameType::DataBlocked:
            case QuicFrameType::StreamsBlockedBidi:
            case QuicFrameType::StreamsBlockedUni:
                break;
            case QuicFrameType::NewToken:
            case QuicFrameType::NewConnectionId:
            case QuicFrameType::RetireConnectionId:
            case QuicFrameType::PathChallenge:
            case QuicFrameType::PathResponse:
            case QuicFrameType::HandshakeDone:
            case QuicFrameType::Stream1:
            case QuicFrameType::Stream2:
            case QuicFrameType::Stream3:
            case QuicFrameType::Stream4:
            case QuicFrameType::Stream5:
            case QuicFrameType::Stream6:
            case QuicFrameType::Stream7:
                break;
        }
    }

    // Track ACK ranges for this received packet.
    // on_packet_received handles forced ACK creation internally when range
    // overflow or too-old conditions occur (mirrors ngx_quic_ack_packet).
    const QuicTime received_time = quic_time_ms(datagram.received_at);
    space.on_packet_received(packet.packet_number, received_time, result.ack_eliciting);

    auto acked = handle_ack_eliciting_packet(space, packet, datagram, previous_largest_received, result.ack_eliciting);
    if (!acked) {
        return std::unexpected(acked.error());
    }
    result.send_ack = space.send_ack;

    if (conn.state() == QuicConnectionState::Init && packet.level == QuicEncryptionLevel::Initial) {
        auto started = conn.start_handshake();
        if (!started) {
            return std::unexpected(started.error());
        }
    }
    if (packet.level == QuicEncryptionLevel::Handshake) {
        discard_packet_number_space(conn, QuicEncryptionLevel::Initial);
        if (result.path != nullptr) {
            result.path->validated = true;
        }
    }

    if (result.path != conn.active_path() && result.non_probing &&
        packet.packet_number == space.largest_received_packet_number) {
        QuicPath *old = conn.active_path();
        if (old != nullptr && !old->validated) {
            conn.free_path(*old);
        }
        if (!conn.set_active_path(*result.path)) {
            return std::unexpected(common::IoErr::Invalid);
        }
    }

    return result;
}

} // namespace

common::IoResult<QuicPacketProcessResult> quic_process_initial_datagram(QuicConnection &conn,
                                                                        const QuicReceivedDatagram &datagram,
                                                                        std::uint8_t *plaintext,
                                                                        std::size_t plaintext_cap) noexcept {
    if (conn.role() != QuicConnectionRole::Server || datagram.data == nullptr ||
        datagram.len < kMinInitialDatagramSize || plaintext == nullptr || plaintext_cap == 0) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (conn.closing()) {
        return std::unexpected(common::IoErr::Canceled);
    }

    auto packet = quic_parse_packet_header(datagram.data, datagram.len, 0);
    if (!packet) {
        return std::unexpected(packet.error());
    }
    if (!packet->long_header || packet->type != QuicPacketType::Initial ||
        packet->level != QuicEncryptionLevel::Initial || packet->version != kQuicVersion1) {
        return std::unexpected(common::IoErr::Invalid);
    }

    if (!conn.crypto().initial_ready) {
        auto initialized = conn.init_initial_crypto(packet->dcid);
        if (!initialized) {
            return std::unexpected(initialized.error());
        }
    }

    auto &space = conn.packet_number_space(packet->level);
    const std::uint64_t previous_largest_received = space.largest_received_packet_number;

    auto opened =
            quic_decrypt_initial_packet(conn, *packet, datagram.data, packet->packet_len, plaintext, plaintext_cap);
    if (!opened) {
        return std::unexpected(opened.error());
    }

    auto bound_path = bind_datagram_path(conn, datagram, *packet, datagram.len);
    if (!bound_path) {
        return std::unexpected(bound_path.error());
    }

    QuicPacketProcessResult result{};
    result.path = *bound_path;
    result.created_path = result.path->tag == QuicPathTag::Probe;
    result.rebound =
            result.created_path && (connection_id_equal(packet->dcid, conn.local_connection_id()) ||
                                    connection_id_equal(packet->dcid, conn.original_destination_connection_id()));
    result.packet_type = packet->type;
    result.level = packet->level;
    result.packet_number = packet->packet_number;

    QuicReadCursor payload(opened->data, opened->len);
    while (!payload.empty()) {
        auto parsed = quic_parse_frame(QuicEncryptionLevel::Initial, payload);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        if (parsed->frame.type == QuicFrameType::Ack || parsed->frame.type == QuicFrameType::AckEcn) {
            auto acked = quic_handle_ack_frame(conn, packet->level, parsed->frame, quic_time_ms(datagram.received_at));
            if (!acked) {
                return std::unexpected(acked.error());
            }
            result.send_output = result.send_output || acked->unblocked || acked->lost_frames || acked->force_send;
        }
        ++result.frame_count;
        result.ack_eliciting = result.ack_eliciting || parsed->frame.ack_eliciting;
        result.non_probing = result.non_probing || !probing_frame(parsed->frame.type);
    }

    if (conn.state() == QuicConnectionState::Init) {
        auto started = conn.start_handshake();
        if (!started) {
            return std::unexpected(started.error());
        }
    }

    if (result.ack_eliciting) {
        const QuicTime now = quic_time_ms(datagram.received_at);
        quic_record_ack_eliciting_received(space, packet->packet_number, now, previous_largest_received);
    }

    // Track ACK ranges for every valid received packet.
    // on_packet_received handles forced ACK creation internally when range
    // overflow or too-old conditions occur (mirrors ngx_quic_ack_packet).
    const QuicTime received_time = quic_time_ms(datagram.received_at);
    space.on_packet_received(packet->packet_number, received_time, result.ack_eliciting);
    result.send_ack = space.send_ack;

    if (result.path != conn.active_path() && result.non_probing &&
        packet->packet_number == space.largest_received_packet_number) {
        QuicPath *old = conn.active_path();
        if (old != nullptr && !old->validated) {
            conn.free_path(*old);
        }
        if (!conn.set_active_path(*result.path)) {
            return std::unexpected(common::IoErr::Invalid);
        }
    }

    return result;
}

common::IoResult<QuicPacketProcessResult> quic_process_datagram(QuicConnection &conn,
                                                                const QuicReceivedDatagram &datagram,
                                                                std::uint8_t *plaintext, std::size_t plaintext_cap,
                                                                std::uint8_t short_dcid_len) noexcept {
    if (datagram.data == nullptr || datagram.len == 0 || plaintext == nullptr || plaintext_cap == 0 || conn.closing()) {
        return std::unexpected(conn.closing() ? common::IoErr::Canceled : common::IoErr::Invalid);
    }

    QuicPacketProcessResult aggregate{};
    bool has_good_packet = false;
    bool recorded_datagram_bytes = false;
    std::size_t offset = 0;

    while (offset < datagram.len) {
        std::uint8_t *packet_data = datagram.data + offset;
        const std::size_t remaining = datagram.len - offset;
        auto packet = quic_parse_packet_header(packet_data, remaining, short_dcid_len);
        if (!packet) {
            if (has_good_packet) {
                return aggregate;
            }
            return std::unexpected(packet.error());
        }

        if (packet->long_header && packet->version != kQuicVersion1) {
            if (has_good_packet) {
                return aggregate;
            }
            return std::unexpected(common::IoErr::NotSupported);
        }
        if (packet->type == QuicPacketType::Retry || packet->type == QuicPacketType::VersionNegotiation) {
            if (has_good_packet) {
                return aggregate;
            }
            return std::unexpected(common::IoErr::Invalid);
        }
        if (packet->packet_len == 0 || packet->packet_len > remaining) {
            if (has_good_packet) {
                return aggregate;
            }
            return std::unexpected(common::IoErr::Invalid);
        }

        if (packet->level == QuicEncryptionLevel::Initial && !conn.crypto().initial_ready) {
            auto initialized = conn.init_initial_crypto(packet->dcid);
            if (!initialized) {
                if (has_good_packet) {
                    return aggregate;
                }
                return std::unexpected(initialized.error());
            }
        }

        QuicPacketNumberSpace &space = conn.packet_number_space(packet->level);
        const std::uint64_t previous_largest_received = space.largest_received_packet_number;
        auto decoded =
                quic_decode_packet(conn, packet_data, packet->packet_len, short_dcid_len, plaintext, plaintext_cap);
        if (!decoded) {
            if (decoded.error() == common::IoErr::NotFound) {
                offset += packet->packet_len;
                continue;
            }
            if (has_good_packet) {
                return aggregate;
            }
            return std::unexpected(decoded.error());
        }

        const std::size_t received_len = recorded_datagram_bytes ? 0 : datagram.len;
        auto processed = process_decoded_packet(conn, datagram, *decoded, received_len, previous_largest_received);
        if (!processed) {
            return std::unexpected(processed.error());
        }
        recorded_datagram_bytes = true;
        has_good_packet = true;

        aggregate.path = processed->path;
        aggregate.packet_type = processed->packet_type;
        aggregate.level = processed->level;
        aggregate.packet_number = processed->packet_number;
        aggregate.packet_count += processed->packet_count;
        aggregate.frame_count += processed->frame_count;
        aggregate.ack_eliciting = aggregate.ack_eliciting || processed->ack_eliciting;
        aggregate.send_ack = aggregate.send_ack || processed->send_ack;
        aggregate.send_output = aggregate.send_output || processed->send_output;
        aggregate.non_probing = aggregate.non_probing || processed->non_probing;
        aggregate.created_path = aggregate.created_path || processed->created_path;
        aggregate.rebound = aggregate.rebound || processed->rebound;

        offset += packet->packet_len;
    }

    if (!has_good_packet) {
        return std::unexpected(common::IoErr::NotFound);
    }
    return aggregate;
}

} // namespace fiber::quic
