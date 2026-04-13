#ifndef FIBER_NET_UDP_SOCKET_H
#define FIBER_NET_UDP_SOCKET_H

#include <cstddef>
#include <cstdint>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../event/EventLoop.h"
#include "SocketAddress.h"
#include "UdpPacket.h"

namespace fiber::net {

struct UdpBindOptions {
    bool reuse_addr = true;
    bool reuse_port = false;
    bool v6_only = false;
    bool recv_packet_info = false;
    bool recv_ecn = false;
};

struct UdpRecvResult {
    size_t size = 0;
    SocketAddress peer{};
};

} // namespace fiber::net

#include "detail/DatagramFd.h"

namespace fiber::net {

class UdpSocket : public common::NonCopyable, public common::NonMovable {
public:
    using RecvFromAwaiter = detail::DatagramFd::RecvFromAwaiter;
    using SendToAwaiter = detail::DatagramFd::SendToAwaiter;
    using RecvPacketAwaiter = detail::DatagramFd::RecvPacketAwaiter;
    using SendPacketAwaiter = detail::DatagramFd::SendPacketAwaiter;
    using WaitEventAwaiter = detail::DatagramFd::WaitEventAwaiter;
    using WaitReadableAwaiter = detail::DatagramFd::WaitReadableAwaiter;
    using WaitWritableAwaiter = detail::DatagramFd::WaitWritableAwaiter;

    explicit UdpSocket(fiber::event::EventLoop &loop);
    ~UdpSocket();

    fiber::common::IoResult<void> bind(const SocketAddress &addr, const UdpBindOptions &options);
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] const SocketAddress &local_addr() const noexcept;
    void close();

    [[nodiscard]] RecvFromAwaiter recv_from(void *buf, size_t len) noexcept;
    [[nodiscard]] SendToAwaiter send_to(const void *buf, size_t len, const SocketAddress &peer) noexcept;
    [[nodiscard]] fiber::common::IoResult<UdpRecvResult> try_recv_from(void *buf, size_t len) noexcept;
    [[nodiscard]] fiber::common::IoResult<size_t> try_send_to(const void *buf, size_t len,
                                                              const SocketAddress &peer) noexcept;

    [[nodiscard]] RecvPacketAwaiter recv_packet(void *buf, size_t len) noexcept;
    [[nodiscard]] SendPacketAwaiter send_packet(UdpPacketSendSpec spec) noexcept;
    [[nodiscard]] fiber::common::IoResult<UdpPacketRecvResult> try_recv_packet(void *buf, size_t len) noexcept;
    [[nodiscard]] fiber::common::IoResult<size_t> try_send_packet(const UdpPacketSendSpec &spec) noexcept;
    [[nodiscard]] fiber::common::IoResult<size_t> try_send_packets(const UdpPacketSendSpec *specs,
                                                                   size_t count) noexcept;
    [[nodiscard]] WaitEventAwaiter wait_event(fiber::event::IoEvent interested) noexcept;
    [[nodiscard]] WaitReadableAwaiter wait_readable() noexcept;
    [[nodiscard]] WaitWritableAwaiter wait_writable() noexcept;
    fiber::async::Task<fiber::common::IoResult<fiber::event::IoEvent>>
    wait_event(fiber::event::IoEvent interested, std::chrono::milliseconds timeout) noexcept;

private:
    detail::DatagramFd socket_;
};

} // namespace fiber::net

#endif // FIBER_NET_UDP_SOCKET_H
