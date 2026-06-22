#ifndef FIBER_QUIC_QUIC_PATH_H
#define FIBER_QUIC_QUIC_PATH_H

#include <cstddef>
#include <cstdint>

#include "../net/SocketAddress.h"
#include "QuicCongestion.h"
#include "QuicConnectionId.h"
#include "QuicFrame.h"

namespace fiber::quic {

inline constexpr std::size_t kQuicMaxPaths = 3;
inline constexpr std::size_t kQuicPathRetries = 3;

enum class QuicPathTag : std::uint8_t {
    Probe,
    Active,
    Backup,
};

enum class QuicPathState : std::uint8_t {
    Idle,
    Validating,
    WaitingMtuProbe,
    MtuDiscovery,
};

struct QuicPath {
    net::SocketAddress remote{};
    net::SocketAddress local{};
    QuicConnectionId remote_connection_id{};
    std::uint64_t remote_connection_id_sequence = 0;
    QuicPathState state = QuicPathState::Idle;
    QuicTime expires{0};
    std::uint32_t tries = 0;
    QuicPathTag tag = QuicPathTag::Probe;
    std::size_t mtu = kQuicCongestionMinInitialSize;
    std::size_t mtud = 0;
    std::size_t max_mtu = 0;
    std::uint64_t sent = 0;
    std::uint64_t received = 0;
    std::uint8_t challenge[2][8]{};
    std::uint64_t seqnum = 0;
    std::uint64_t mtu_packet_numbers[kQuicPathRetries]{};
    QuicOutputFrameQueue pending_frames{};
    bool allocated = false;
    bool validated = false;
    bool mtu_unvalidated = false;
    bool used = false;
};

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_PATH_H
