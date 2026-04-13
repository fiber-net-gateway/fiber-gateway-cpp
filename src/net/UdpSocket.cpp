#include "UdpSocket.h"

#include <utility>

namespace fiber::net {

UdpSocket::UdpSocket(fiber::event::EventLoop &loop) : socket_(loop) {}

UdpSocket::~UdpSocket() {}

fiber::common::IoResult<void> UdpSocket::bind(const SocketAddress &addr, const UdpBindOptions &options) {
    return socket_.bind(addr, options);
}

bool UdpSocket::valid() const noexcept { return socket_.valid(); }

int UdpSocket::fd() const noexcept { return socket_.fd(); }

const SocketAddress &UdpSocket::local_addr() const noexcept { return socket_.local_addr(); }

void UdpSocket::close() { socket_.close(); }

UdpSocket::RecvFromAwaiter UdpSocket::recv_from(void *buf, size_t len) noexcept { return socket_.recv_from(buf, len); }

UdpSocket::SendToAwaiter UdpSocket::send_to(const void *buf, size_t len, const SocketAddress &peer) noexcept {
    return socket_.send_to(buf, len, peer);
}

fiber::common::IoResult<UdpRecvResult> UdpSocket::try_recv_from(void *buf, size_t len) noexcept {
    return socket_.try_recv_from(buf, len);
}

fiber::common::IoResult<size_t> UdpSocket::try_send_to(const void *buf, size_t len,
                                                       const SocketAddress &peer) noexcept {
    return socket_.try_send_to(buf, len, peer);
}

UdpSocket::RecvPacketAwaiter UdpSocket::recv_packet(void *buf, size_t len) noexcept {
    return socket_.recv_packet(buf, len);
}

UdpSocket::SendPacketAwaiter UdpSocket::send_packet(UdpPacketSendSpec spec) noexcept {
    return socket_.send_packet(std::move(spec));
}

fiber::common::IoResult<UdpPacketRecvResult> UdpSocket::try_recv_packet(void *buf, size_t len) noexcept {
    return socket_.try_recv_packet(buf, len);
}

fiber::common::IoResult<size_t> UdpSocket::try_send_packet(const UdpPacketSendSpec &spec) noexcept {
    return socket_.try_send_packet(spec);
}

fiber::common::IoResult<size_t> UdpSocket::try_send_packets(const UdpPacketSendSpec *specs, size_t count) noexcept {
    if ((count != 0 && specs == nullptr) || !valid()) {
        return std::unexpected(fiber::common::IoErr::Invalid);
    }

    size_t sent = 0;
    for (; sent < count; ++sent) {
        auto result = socket_.try_send_packet(specs[sent]);
        if (result) {
            continue;
        }
        if (result.error() == fiber::common::IoErr::WouldBlock && sent != 0) {
            break;
        }
        if (sent != 0) {
            break;
        }
        return std::unexpected(result.error());
    }
    return sent;
}

UdpSocket::WaitEventAwaiter UdpSocket::wait_event(fiber::event::IoEvent interested) noexcept {
    return socket_.wait_event(interested);
}

UdpSocket::WaitReadableAwaiter UdpSocket::wait_readable() noexcept { return socket_.wait_readable(); }

UdpSocket::WaitWritableAwaiter UdpSocket::wait_writable() noexcept { return socket_.wait_writable(); }

fiber::async::Task<fiber::common::IoResult<fiber::event::IoEvent>>
UdpSocket::wait_event(fiber::event::IoEvent interested, std::chrono::milliseconds timeout) noexcept {
    co_return co_await socket_.wait_event(interested, timeout);
}

} // namespace fiber::net
