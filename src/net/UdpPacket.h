#ifndef FIBER_NET_UDP_PACKET_H
#define FIBER_NET_UDP_PACKET_H

#include <cstddef>
#include <cstdint>
#include <sys/uio.h>

#include "SocketAddress.h"

namespace fiber::net {

inline constexpr std::size_t kUdpMaxBatchSize = 64;

enum class UdpEcn : std::int8_t {
    Unspecified = -1,
    NonEct = 0,
    Ect1 = 1,
    Ect0 = 2,
    Ce = 3,
};

struct UdpPacketRecvResult {
    size_t size = 0;
    SocketAddress peer{};
    SocketAddress local{};
    UdpEcn ecn = UdpEcn::Unspecified;
    bool truncated = false;
};

struct UdpPacketRecvSlot {
    void *buf = nullptr;
    size_t capacity = 0;
    UdpPacketRecvResult result{};
};

struct UdpPacketSendSpec {
    const void *buf = nullptr;
    size_t len = 0;
    const iovec *iov = nullptr;
    int iov_count = 0;
    SocketAddress peer{};
    SocketAddress local{};
    bool has_local = false;
    UdpEcn ecn = UdpEcn::Unspecified;
    std::uint16_t gso_segment_size = 0;
};

} // namespace fiber::net

#endif // FIBER_NET_UDP_PACKET_H
