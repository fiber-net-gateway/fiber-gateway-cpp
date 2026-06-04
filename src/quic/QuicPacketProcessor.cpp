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

[[nodiscard]] bool socket_address_is_unspecified(const net::SocketAddress &addr) noexcept {
    return addr.port() == 0 && addr.ip().is_unspecified();
}

[[nodiscard]] common::IoResult<void> validate_path(const QuicConnection &conn,
                                                   const QuicReceivedDatagram &datagram) noexcept {
    if (!socket_address_is_unspecified(conn.remote_addr()) &&
        !socket_address_equal(conn.remote_addr(), datagram.peer)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (!socket_address_is_unspecified(conn.local_addr()) && !socket_address_equal(conn.local_addr(), datagram.local)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return {};
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

    auto path_valid = validate_path(conn, datagram);
    if (!path_valid) {
        return std::unexpected(path_valid.error());
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

    QuicPacketProcessResult result{};
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
    return result;
}

} // namespace fiber::quic
