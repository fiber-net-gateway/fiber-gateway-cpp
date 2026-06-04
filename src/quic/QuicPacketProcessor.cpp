#include "QuicPacketProcessor.h"

#include <algorithm>
#include <expected>

#include "QuicAckHandler.h"
#include "QuicCrypto.h"
#include "QuicCursor.h"
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
                                                              const QuicPacketHeader &packet) noexcept {
    if (QuicPath *path = conn.find_path(datagram.peer, datagram.local)) {
        conn.record_path_received(*path, datagram.len);
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
    conn.record_path_received(*path, datagram.len);
    return path;
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

    auto opened =
            quic_decrypt_initial_packet(conn, *packet, datagram.data, packet->packet_len, plaintext, plaintext_cap);
    if (!opened) {
        return std::unexpected(opened.error());
    }

    auto bound_path = bind_datagram_path(conn, datagram, *packet);
    if (!bound_path) {
        return std::unexpected(bound_path.error());
    }

    QuicPacketProcessResult result{};
    result.path = *bound_path;
    result.created_path = result.path->tag == QuicPathTag::Probe;
    result.rebound = result.created_path &&
                     (connection_id_equal(packet->dcid, conn.local_connection_id()) ||
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

    auto &space = conn.packet_number_space(QuicEncryptionLevel::Initial);
    if (result.ack_eliciting) {
        space.send_ack = true;
    }
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

} // namespace fiber::quic
