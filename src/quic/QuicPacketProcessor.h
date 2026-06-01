#ifndef FIBER_QUIC_QUIC_PACKET_PROCESSOR_H
#define FIBER_QUIC_QUIC_PACKET_PROCESSOR_H

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "../common/IoError.h"
#include "../net/SocketAddress.h"
#include "../net/UdpPacket.h"
#include "QuicConnection.h"
#include "QuicProtocol.h"

namespace fiber::quic {

struct QuicReceivedDatagram {
    std::uint8_t *data = nullptr;
    std::size_t len = 0;
    net::SocketAddress peer{};
    net::SocketAddress local{};
    net::UdpEcn ecn = net::UdpEcn::Unspecified;
    std::chrono::steady_clock::time_point received_at{};
};

struct QuicPacketProcessResult {
    QuicPacketType packet_type = QuicPacketType::Initial;
    QuicEncryptionLevel level = QuicEncryptionLevel::Initial;
    std::uint64_t packet_number = 0;
    std::uint32_t frame_count = 0;
    bool ack_eliciting = false;
    bool send_ack = false;
};

[[nodiscard]] common::IoResult<QuicPacketProcessResult>
quic_process_initial_datagram(QuicConnection &conn, const QuicReceivedDatagram &datagram, std::uint8_t *plaintext,
                              std::size_t plaintext_cap) noexcept;

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_PACKET_PROCESSOR_H
