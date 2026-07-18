#ifndef FIBER_NET_DETAIL_DATAGRAM_FD_H
#define FIBER_NET_DETAIL_DATAGRAM_FD_H

#include <chrono>
#include <cstddef>

#include "../../async/Task.h"
#include "../../common/Assert.h"
#include "../../common/IoError.h"
#include "../../common/NonCopyable.h"
#include "../../common/NonMovable.h"
#include "../../event/EventLoop.h"
#include "../SocketAddress.h"
#include "../UdpPacket.h"
#include "RWFd.h"

namespace fiber::net {

struct UdpBindOptions;
struct UdpRecvResult;

} // namespace fiber::net

namespace fiber::net::detail {

class DatagramFd : public common::NonCopyable, public common::NonMovable {
public:
    using RecvFromTask = fiber::async::Task<fiber::common::IoResult<UdpRecvResult>>;
    using SendTask = fiber::async::Task<fiber::common::IoResult<size_t>>;
    using RecvPacketTask = fiber::async::Task<fiber::common::IoResult<UdpPacketRecvResult>>;
    using WaitReadableAwaiter = RWFd::WaitReadableAwaiter;
    using WaitWritableAwaiter = RWFd::WaitWritableAwaiter;
    using WaitTask = RWFd::WaitTask;

    explicit DatagramFd(fiber::event::EventLoop &loop);
    ~DatagramFd();

    fiber::common::IoResult<void> bind(const SocketAddress &addr, const UdpBindOptions &options);
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] const SocketAddress &local_addr() const noexcept;
    void close();

    [[nodiscard]] RecvFromTask recv_from(void *buf, size_t len,
                                         std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    [[nodiscard]] SendTask send_to(const void *buf, size_t len, const SocketAddress &peer,
                                   std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    [[nodiscard]] fiber::common::IoResult<UdpRecvResult> try_recv_from(void *buf, size_t len) noexcept;
    [[nodiscard]] fiber::common::IoResult<size_t> try_send_to(const void *buf, size_t len,
                                                              const SocketAddress &peer) noexcept;

    [[nodiscard]] RecvPacketTask
    recv_packet(void *buf, size_t len, std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    [[nodiscard]] SendTask send_packet(UdpPacketSendSpec spec,
                                       std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    [[nodiscard]] fiber::common::IoResult<UdpPacketRecvResult> try_recv_packet(void *buf, size_t len) noexcept;
    [[nodiscard]] fiber::common::IoResult<size_t> try_send_packet(const UdpPacketSendSpec &spec) noexcept;
    [[nodiscard]] WaitReadableAwaiter wait_readable() noexcept;
    [[nodiscard]] WaitWritableAwaiter wait_writable() noexcept;
    [[nodiscard]] WaitTask wait_readable(std::chrono::milliseconds timeout) noexcept;
    [[nodiscard]] WaitTask wait_writable(std::chrono::milliseconds timeout) noexcept;

private:
    fiber::common::IoResult<void> refresh_local_addr() noexcept;
    fiber::common::IoErr recv_packet_once(void *buf, size_t len, UdpPacketRecvResult &out) noexcept;
    fiber::common::IoErr send_packet_once(const UdpPacketSendSpec &spec, size_t &out) noexcept;

    RWFd rwfd_;
    SocketAddress local_addr_{};
};

} // namespace fiber::net::detail

#endif // FIBER_NET_DETAIL_DATAGRAM_FD_H
