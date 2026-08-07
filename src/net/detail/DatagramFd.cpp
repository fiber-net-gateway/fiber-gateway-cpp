#include <fiber/net/detail/DatagramFd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#if FIBER_HAVE_UDP_SEGMENT
#include <netinet/udp.h>
#endif
#include <sys/socket.h>
#include <unistd.h>

#include <fiber/net/UdpSocket.h>

namespace fiber::net::detail {

namespace {

using Deadline = std::chrono::steady_clock::time_point;

Deadline make_deadline(std::chrono::milliseconds timeout) noexcept {
    if (timeout == std::chrono::milliseconds::max()) {
        return Deadline::max();
    }
    return fiber::event::EventLoop::current().now() + timeout;
}

fiber::common::IoResult<std::chrono::milliseconds> remaining_timeout(Deadline deadline) noexcept {
    if (deadline == Deadline::max()) {
        return std::chrono::milliseconds::max();
    }
    auto now = fiber::event::EventLoop::current().now();
    if (deadline <= now) {
        return std::unexpected(fiber::common::IoErr::TimedOut);
    }
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    if (remaining <= std::chrono::milliseconds::zero()) {
        remaining = std::chrono::milliseconds(1);
    }
    return remaining;
}

constexpr std::size_t kRecvControlCapacity =
        CMSG_SPACE(sizeof(in6_pktinfo)) + CMSG_SPACE(sizeof(in_pktinfo)) + CMSG_SPACE(sizeof(int));
constexpr std::size_t kSendControlCapacity = CMSG_SPACE(sizeof(in6_pktinfo)) + CMSG_SPACE(sizeof(in_pktinfo)) +
                                             CMSG_SPACE(sizeof(int)) + CMSG_SPACE(sizeof(std::uint16_t));

template<std::size_t Capacity>
struct alignas(cmsghdr) ControlBuffer {
    std::array<unsigned char, Capacity> bytes{};
};

fiber::common::IoErr set_sockopt_flag(int fd, int level, int option, bool enabled) noexcept {
    int value = enabled ? 1 : 0;
    if (::setsockopt(fd, level, option, &value, sizeof(value)) != 0) {
        return fiber::common::io_err_from_errno(errno);
    }
    return fiber::common::IoErr::None;
}

fiber::common::IoErr configure_packet_options(int fd, int domain, const UdpBindOptions &options) noexcept {
    if (options.reuse_addr) {
        fiber::common::IoErr err = set_sockopt_flag(fd, SOL_SOCKET, SO_REUSEADDR, true);
        if (err != fiber::common::IoErr::None) {
            return err;
        }
    }
#ifdef SO_REUSEPORT
    if (options.reuse_port) {
        fiber::common::IoErr err = set_sockopt_flag(fd, SOL_SOCKET, SO_REUSEPORT, true);
        if (err != fiber::common::IoErr::None) {
            return err;
        }
    }
#else
    if (options.reuse_port) {
        return fiber::common::IoErr::NotSupported;
    }
#endif

    if (domain == AF_INET6 && options.v6_only) {
        fiber::common::IoErr err = set_sockopt_flag(fd, IPPROTO_IPV6, IPV6_V6ONLY, true);
        if (err != fiber::common::IoErr::None) {
            return err;
        }
    }

    if (options.recv_packet_info) {
        if (domain == AF_INET) {
            fiber::common::IoErr err = set_sockopt_flag(fd, IPPROTO_IP, IP_PKTINFO, true);
            if (err != fiber::common::IoErr::None) {
                return err;
            }
        } else {
            fiber::common::IoErr err = set_sockopt_flag(fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, true);
            if (err != fiber::common::IoErr::None) {
                return err;
            }
#ifdef IP_PKTINFO
            err = set_sockopt_flag(fd, IPPROTO_IP, IP_PKTINFO, true);
            if (err != fiber::common::IoErr::None) {
                return err;
            }
#endif
        }
    }

    if (options.recv_ecn) {
        if (domain == AF_INET) {
#ifdef IP_RECVTOS
            fiber::common::IoErr err = set_sockopt_flag(fd, IPPROTO_IP, IP_RECVTOS, true);
            if (err != fiber::common::IoErr::None) {
                return err;
            }
#else
            return fiber::common::IoErr::NotSupported;
#endif
        } else {
#ifdef IPV6_RECVTCLASS
            fiber::common::IoErr err = set_sockopt_flag(fd, IPPROTO_IPV6, IPV6_RECVTCLASS, true);
            if (err != fiber::common::IoErr::None) {
                return err;
            }
#else
            return fiber::common::IoErr::NotSupported;
#endif
#ifdef IP_RECVTOS
            err = set_sockopt_flag(fd, IPPROTO_IP, IP_RECVTOS, true);
            if (err != fiber::common::IoErr::None) {
                return err;
            }
#endif
        }
    }

    return fiber::common::IoErr::None;
}

void fill_default_local_from_bound(const SocketAddress &bound, UdpPacketRecvResult &result) noexcept {
    result.local = bound;
}

void apply_ipv4_local(sockaddr_storage &storage, std::uint16_t port, const in_addr &addr) noexcept {
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr = addr;
    std::memset(&storage, 0, sizeof(storage));
    std::memcpy(&storage, &sa, sizeof(sa));
}

void apply_ipv6_local(sockaddr_storage &storage, std::uint16_t port, const in6_addr &addr,
                      std::uint32_t scope_id) noexcept {
    sockaddr_in6 sa{};
    sa.sin6_family = AF_INET6;
    sa.sin6_port = htons(port);
    sa.sin6_addr = addr;
    sa.sin6_scope_id = scope_id;
    std::memset(&storage, 0, sizeof(storage));
    std::memcpy(&storage, &sa, sizeof(sa));
}

void parse_control_messages(const msghdr &msg, const SocketAddress &bound, UdpPacketRecvResult &result) noexcept {
    fill_default_local_from_bound(bound, result);
    result.ecn = UdpEcn::Unspecified;

    sockaddr_storage local_storage{};
    bool local_ready = false;

    for (cmsghdr *cmsg = CMSG_FIRSTHDR(const_cast<msghdr *>(&msg)); cmsg;
         cmsg = CMSG_NXTHDR(const_cast<msghdr *>(&msg), cmsg)) {
        if (cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_PKTINFO &&
            cmsg->cmsg_len >= CMSG_LEN(sizeof(in_pktinfo))) {
            const auto *pkt = reinterpret_cast<const in_pktinfo *>(CMSG_DATA(cmsg));
            apply_ipv4_local(local_storage, bound.port(), pkt->ipi_addr);
            local_ready = true;
            continue;
        }
        if (cmsg->cmsg_level == IPPROTO_IPV6 && cmsg->cmsg_type == IPV6_PKTINFO &&
            cmsg->cmsg_len >= CMSG_LEN(sizeof(in6_pktinfo))) {
            const auto *pkt6 = reinterpret_cast<const in6_pktinfo *>(CMSG_DATA(cmsg));
            apply_ipv6_local(local_storage, bound.port(), pkt6->ipi6_addr, pkt6->ipi6_ifindex);
            local_ready = true;
            continue;
        }
#ifdef IP_TOS
        if (cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_TOS) {
            if (cmsg->cmsg_len >= CMSG_LEN(sizeof(int))) {
                int tos = *reinterpret_cast<const int *>(CMSG_DATA(cmsg));
                result.ecn = static_cast<UdpEcn>(tos & 0x03);
            } else if (cmsg->cmsg_len >= CMSG_LEN(sizeof(unsigned char))) {
                unsigned char tos = *reinterpret_cast<const unsigned char *>(CMSG_DATA(cmsg));
                result.ecn = static_cast<UdpEcn>(tos & 0x03);
            }
            continue;
        }
#endif
#ifdef IPV6_TCLASS
        if (cmsg->cmsg_level == IPPROTO_IPV6 && cmsg->cmsg_type == IPV6_TCLASS &&
            cmsg->cmsg_len >= CMSG_LEN(sizeof(int))) {
            int tclass = *reinterpret_cast<const int *>(CMSG_DATA(cmsg));
            result.ecn = static_cast<UdpEcn>(tclass & 0x03);
            continue;
        }
#endif
    }

    if (local_ready) {
        SocketAddress parsed;
        if (SocketAddress::from_sockaddr(reinterpret_cast<const sockaddr *>(&local_storage),
                                         static_cast<socklen_t>(sizeof(local_storage)), parsed)) {
            result.local = parsed;
        }
    }
}

std::size_t build_send_control(const UdpPacketSendSpec &spec, std::array<unsigned char, kSendControlCapacity> &control,
                               msghdr &msg) noexcept {
    msg.msg_control = nullptr;
    msg.msg_controllen = 0;
    if (!spec.has_local && spec.ecn == UdpEcn::Unspecified && spec.gso_segment_size == 0) {
        return 0;
    }

    control.fill(0);
    msg.msg_control = control.data();
    msg.msg_controllen = control.size();
    cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg) {
        msg.msg_control = nullptr;
        msg.msg_controllen = 0;
        return 0;
    }

    unsigned char *used_end = control.data();
    auto mark_used = [&](cmsghdr *cur) noexcept {
        used_end = reinterpret_cast<unsigned char *>(cur) + CMSG_SPACE(cur->cmsg_len - CMSG_LEN(0));
    };
    auto advance = [&](cmsghdr *cur) -> cmsghdr * {
        mark_used(cur);
        return CMSG_NXTHDR(&msg, cur);
    };

    if (spec.has_local) {
        sockaddr_storage storage{};
        socklen_t addr_len = 0;
        if (!spec.local.to_sockaddr(storage, addr_len)) {
            msg.msg_control = nullptr;
            msg.msg_controllen = 0;
            return 0;
        }
        const auto *sa = reinterpret_cast<const sockaddr *>(&storage);
        if (sa->sa_family == AF_INET) {
            const auto *in = reinterpret_cast<const sockaddr_in *>(sa);
            cmsg->cmsg_level = IPPROTO_IP;
            cmsg->cmsg_type = IP_PKTINFO;
            cmsg->cmsg_len = CMSG_LEN(sizeof(in_pktinfo));
            auto *pkt = reinterpret_cast<in_pktinfo *>(CMSG_DATA(cmsg));
            std::memset(pkt, 0, sizeof(*pkt));
            pkt->ipi_spec_dst = in->sin_addr;
            cmsg = advance(cmsg);
            if (!cmsg && spec.ecn != UdpEcn::Unspecified) {
                return 0;
            }
        } else if (sa->sa_family == AF_INET6) {
            const auto *in6 = reinterpret_cast<const sockaddr_in6 *>(sa);
            cmsg->cmsg_level = IPPROTO_IPV6;
            cmsg->cmsg_type = IPV6_PKTINFO;
            cmsg->cmsg_len = CMSG_LEN(sizeof(in6_pktinfo));
            auto *pkt6 = reinterpret_cast<in6_pktinfo *>(CMSG_DATA(cmsg));
            std::memset(pkt6, 0, sizeof(*pkt6));
            pkt6->ipi6_addr = in6->sin6_addr;
            pkt6->ipi6_ifindex = in6->sin6_scope_id;
            cmsg = advance(cmsg);
            if (!cmsg && spec.ecn != UdpEcn::Unspecified) {
                return 0;
            }
        }
    }

    if (spec.ecn != UdpEcn::Unspecified) {
        int ecn = static_cast<int>(spec.ecn) & 0x03;
        if (!cmsg) {
            return 0;
        }
        if (spec.peer.family() == IpFamily::V4) {
            cmsg->cmsg_level = IPPROTO_IP;
            cmsg->cmsg_type = IP_TOS;
            cmsg->cmsg_len = CMSG_LEN(sizeof(ecn));
            std::memcpy(CMSG_DATA(cmsg), &ecn, sizeof(ecn));
            mark_used(cmsg);
        } else {
            cmsg->cmsg_level = IPPROTO_IPV6;
            cmsg->cmsg_type = IPV6_TCLASS;
            cmsg->cmsg_len = CMSG_LEN(sizeof(ecn));
            std::memcpy(CMSG_DATA(cmsg), &ecn, sizeof(ecn));
            mark_used(cmsg);
        }
    } else if (spec.has_local) {
        used_end = std::max(used_end, control.data());
    }

    if (spec.gso_segment_size != 0) {
#if FIBER_HAVE_UDP_SEGMENT
        if (spec.ecn != UdpEcn::Unspecified) {
            cmsg = advance(cmsg);
        }
        if (!cmsg) {
            return 0;
        }
        cmsg->cmsg_level = SOL_UDP;
        cmsg->cmsg_type = UDP_SEGMENT;
        cmsg->cmsg_len = CMSG_LEN(sizeof(spec.gso_segment_size));
        std::memcpy(CMSG_DATA(cmsg), &spec.gso_segment_size, sizeof(spec.gso_segment_size));
        mark_used(cmsg);
#endif
    }

    msg.msg_controllen = static_cast<std::size_t>(used_end - control.data());
    return msg.msg_controllen;
}

bool send_spec_valid(const UdpPacketSendSpec &spec) noexcept {
    if (spec.iov_count < 0) {
        return false;
    }
    if (spec.iov_count > 0) {
        return spec.iov != nullptr;
    }
    return spec.len == 0 || spec.buf != nullptr;
}

bool send_spec_supported(const UdpPacketSendSpec &spec) noexcept {
#if FIBER_HAVE_UDP_SEGMENT
    (void) spec;
    return true;
#else
    return spec.gso_segment_size == 0;
#endif
}

} // namespace

DatagramFd::DatagramFd(fiber::event::EventLoop &loop) : rwfd_(loop) {}

DatagramFd::~DatagramFd() {
    if (!rwfd_.valid()) {
        return;
    }
    if (rwfd_.loop().in_loop()) {
        close();
        return;
    }
    FIBER_ASSERT(false);
}

fiber::common::IoResult<void> DatagramFd::bind(const SocketAddress &addr, const UdpBindOptions &options) {
    if (rwfd_.valid()) {
        return std::unexpected(fiber::common::IoErr::Already);
    }

    sockaddr_storage storage{};
    socklen_t len = 0;
    if (!addr.to_sockaddr(storage, len)) {
        return std::unexpected(fiber::common::IoErr::NotSupported);
    }

    int domain = addr.family() == IpFamily::V4 ? AF_INET : AF_INET6;
    int fd = ::socket(domain, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return std::unexpected(fiber::common::io_err_from_errno(errno));
    }

    fiber::common::IoErr err = configure_packet_options(fd, domain, options);
    if (err != fiber::common::IoErr::None) {
        ::close(fd);
        return std::unexpected(err);
    }

    if (::bind(fd, reinterpret_cast<const sockaddr *>(&storage), len) != 0) {
        err = fiber::common::io_err_from_errno(errno);
        ::close(fd);
        return std::unexpected(err);
    }

    err = rwfd_.attach(fd);
    if (err != fiber::common::IoErr::None) {
        ::close(fd);
        return std::unexpected(err);
    }

    auto local_result = refresh_local_addr();
    if (!local_result) {
        rwfd_.close();
        return std::unexpected(local_result.error());
    }
    return {};
}

bool DatagramFd::valid() const noexcept { return rwfd_.valid(); }

int DatagramFd::fd() const noexcept { return rwfd_.fd(); }

const SocketAddress &DatagramFd::local_addr() const noexcept { return local_addr_; }

void DatagramFd::close() { rwfd_.close(); }

fiber::common::IoResult<void> DatagramFd::refresh_local_addr() noexcept {
    sockaddr_storage bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(rwfd_.fd(), reinterpret_cast<sockaddr *>(&bound), &len) != 0) {
        return std::unexpected(fiber::common::io_err_from_errno(errno));
    }
    if (!SocketAddress::from_sockaddr(reinterpret_cast<const sockaddr *>(&bound), len, local_addr_)) {
        return std::unexpected(fiber::common::IoErr::NotSupported);
    }
    return {};
}

DatagramFd::RecvFromTask DatagramFd::recv_from(void *buf, size_t len, std::chrono::milliseconds timeout) noexcept {
    Deadline deadline = make_deadline(timeout);
    for (;;) {
        auto result = try_recv_from(buf, len);
        if (result || result.error() != fiber::common::IoErr::WouldBlock) {
            co_return result;
        }
        auto remaining = remaining_timeout(deadline);
        if (!remaining) {
            co_return std::unexpected(remaining.error());
        }
        auto wait_result = co_await rwfd_.wait_readable(*remaining);
        if (!wait_result) {
            co_return std::unexpected(wait_result.error());
        }
    }
}

DatagramFd::SendTask DatagramFd::send_to(const void *buf, size_t len, const SocketAddress &peer,
                                         std::chrono::milliseconds timeout) noexcept {
    Deadline deadline = make_deadline(timeout);
    for (;;) {
        auto result = try_send_to(buf, len, peer);
        if (result || result.error() != fiber::common::IoErr::WouldBlock) {
            co_return result;
        }
        auto remaining = remaining_timeout(deadline);
        if (!remaining) {
            co_return std::unexpected(remaining.error());
        }
        auto wait_result = co_await rwfd_.wait_writable(*remaining);
        if (!wait_result) {
            co_return std::unexpected(wait_result.error());
        }
    }
}

DatagramFd::RecvPacketTask DatagramFd::recv_packet(void *buf, size_t len, std::chrono::milliseconds timeout) noexcept {
    Deadline deadline = make_deadline(timeout);
    for (;;) {
        auto result = try_recv_packet(buf, len);
        if (result || result.error() != fiber::common::IoErr::WouldBlock) {
            co_return result;
        }
        auto remaining = remaining_timeout(deadline);
        if (!remaining) {
            co_return std::unexpected(remaining.error());
        }
        auto wait_result = co_await rwfd_.wait_readable(*remaining);
        if (!wait_result) {
            co_return std::unexpected(wait_result.error());
        }
    }
}

DatagramFd::SendTask DatagramFd::send_packet(UdpPacketSendSpec spec, std::chrono::milliseconds timeout) noexcept {
    Deadline deadline = make_deadline(timeout);
    for (;;) {
        auto result = try_send_packet(spec);
        if (result || result.error() != fiber::common::IoErr::WouldBlock) {
            co_return result;
        }
        auto remaining = remaining_timeout(deadline);
        if (!remaining) {
            co_return std::unexpected(remaining.error());
        }
        auto wait_result = co_await rwfd_.wait_writable(*remaining);
        if (!wait_result) {
            co_return std::unexpected(wait_result.error());
        }
    }
}

DatagramFd::WaitReadableAwaiter DatagramFd::wait_readable(std::chrono::milliseconds timeout) noexcept {
    return rwfd_.wait_readable(timeout);
}

DatagramFd::WaitWritableAwaiter DatagramFd::wait_writable(std::chrono::milliseconds timeout) noexcept {
    return rwfd_.wait_writable(timeout);
}

fiber::common::IoResult<UdpRecvResult> DatagramFd::try_recv_from(void *buf, size_t len) noexcept {
    auto packet_result = try_recv_packet(buf, len);
    if (!packet_result) {
        return std::unexpected(packet_result.error());
    }
    return UdpRecvResult{packet_result->size, packet_result->peer};
}

fiber::common::IoResult<size_t> DatagramFd::try_send_to(const void *buf, size_t len,
                                                        const SocketAddress &peer) noexcept {
    UdpPacketSendSpec spec;
    spec.buf = buf;
    spec.len = len;
    spec.peer = peer;
    return try_send_packet(spec);
}

fiber::common::IoResult<UdpPacketRecvResult> DatagramFd::try_recv_packet(void *buf, size_t len) noexcept {
    UdpPacketRecvResult result{};
    fiber::common::IoErr err = recv_packet_once(buf, len, result);
    if (err != fiber::common::IoErr::None) {
        return std::unexpected(err);
    }
    return result;
}

fiber::common::IoResult<size_t> DatagramFd::try_send_packet(const UdpPacketSendSpec &spec) noexcept {
    size_t out = 0;
    fiber::common::IoErr err = send_packet_once(spec, out);
    if (err != fiber::common::IoErr::None) {
        return std::unexpected(err);
    }
    return out;
}

fiber::common::IoResult<size_t> DatagramFd::try_recv_packets(UdpPacketRecvSlot *slots, size_t count) noexcept {
    const int socket_fd = rwfd_.fd();
    if (socket_fd < 0) {
        return std::unexpected(fiber::common::IoErr::BadFd);
    }
    if ((count != 0 && slots == nullptr) || count > kUdpMaxBatchSize) {
        return std::unexpected(fiber::common::IoErr::Invalid);
    }
    if (count == 0) {
        return 0;
    }

    std::array<mmsghdr, kUdpMaxBatchSize> messages{};
    std::array<iovec, kUdpMaxBatchSize> iovs{};
    std::array<sockaddr_storage, kUdpMaxBatchSize> peers{};
    std::array<ControlBuffer<kRecvControlCapacity>, kUdpMaxBatchSize> controls{};

    for (size_t i = 0; i < count; ++i) {
        if (slots[i].capacity != 0 && slots[i].buf == nullptr) {
            return std::unexpected(fiber::common::IoErr::Invalid);
        }
        slots[i].result = UdpPacketRecvResult{};
        iovs[i].iov_base = slots[i].buf;
        iovs[i].iov_len = slots[i].capacity;
        msghdr &msg = messages[i].msg_hdr;
        msg.msg_name = &peers[i];
        msg.msg_namelen = sizeof(peers[i]);
        msg.msg_iov = &iovs[i];
        msg.msg_iovlen = 1;
        msg.msg_control = controls[i].bytes.data();
        msg.msg_controllen = controls[i].bytes.size();
    }

    for (;;) {
        const int rc = ::recvmmsg(socket_fd, messages.data(), static_cast<unsigned int>(count), MSG_DONTWAIT, nullptr);
        if (rc >= 0) {
            for (int i = 0; i < rc; ++i) {
                SocketAddress peer;
                const msghdr &msg = messages[static_cast<size_t>(i)].msg_hdr;
                if (!SocketAddress::from_sockaddr(reinterpret_cast<const sockaddr *>(&peers[static_cast<size_t>(i)]),
                                                  msg.msg_namelen, peer)) {
                    return std::unexpected(fiber::common::IoErr::NotSupported);
                }
                UdpPacketRecvResult &result = slots[static_cast<size_t>(i)].result;
                result.size = messages[static_cast<size_t>(i)].msg_len;
                result.peer = peer;
                result.truncated = (msg.msg_flags & MSG_TRUNC) != 0;
                parse_control_messages(msg, local_addr_, result);
            }
            return static_cast<size_t>(rc);
        }

        const int err = errno;
        if (err == EINTR) {
            continue;
        }
        if (err == EAGAIN || err == EWOULDBLOCK) {
            return std::unexpected(fiber::common::IoErr::WouldBlock);
        }
        return std::unexpected(fiber::common::io_err_from_errno(err));
    }
}

fiber::common::IoResult<size_t> DatagramFd::try_send_packets(const UdpPacketSendSpec *specs, size_t count) noexcept {
    const int socket_fd = rwfd_.fd();
    if (socket_fd < 0) {
        return std::unexpected(fiber::common::IoErr::BadFd);
    }
    if ((count != 0 && specs == nullptr) || count > kUdpMaxBatchSize) {
        return std::unexpected(fiber::common::IoErr::Invalid);
    }
    if (count == 0) {
        return 0;
    }

    std::array<mmsghdr, kUdpMaxBatchSize> messages{};
    std::array<iovec, kUdpMaxBatchSize> single_iovs{};
    std::array<sockaddr_storage, kUdpMaxBatchSize> peers{};
    std::array<ControlBuffer<kSendControlCapacity>, kUdpMaxBatchSize> controls{};

    for (size_t i = 0; i < count; ++i) {
        const UdpPacketSendSpec &spec = specs[i];
        if (!send_spec_valid(spec)) {
            return std::unexpected(fiber::common::IoErr::Invalid);
        }
        if (!send_spec_supported(spec)) {
            return std::unexpected(fiber::common::IoErr::NotSupported);
        }

        socklen_t peer_len = 0;
        if (!spec.peer.to_sockaddr(peers[i], peer_len)) {
            return std::unexpected(fiber::common::IoErr::NotSupported);
        }

        const iovec *iov = spec.iov;
        int iov_count = spec.iov_count;
        if (iov_count == 0) {
            single_iovs[i].iov_base = const_cast<void *>(spec.buf);
            single_iovs[i].iov_len = spec.len;
            iov = &single_iovs[i];
            iov_count = 1;
        }

        msghdr &msg = messages[i].msg_hdr;
        msg.msg_name = &peers[i];
        msg.msg_namelen = peer_len;
        msg.msg_iov = const_cast<iovec *>(iov);
        msg.msg_iovlen = static_cast<size_t>(iov_count);
        build_send_control(spec, controls[i].bytes, msg);
    }

    for (;;) {
        const int rc = ::sendmmsg(socket_fd, messages.data(), static_cast<unsigned int>(count), MSG_DONTWAIT);
        if (rc >= 0) {
            return static_cast<size_t>(rc);
        }

        const int err = errno;
        if (err == EINTR) {
            continue;
        }
        if (err == EAGAIN || err == EWOULDBLOCK) {
            return std::unexpected(fiber::common::IoErr::WouldBlock);
        }
        return std::unexpected(fiber::common::io_err_from_errno(err));
    }
}

fiber::common::IoErr DatagramFd::set_read_callback(ReadyCallback callback, void *ctx) noexcept {
    return rwfd_.set_read_callback(callback, ctx);
}

fiber::common::IoErr DatagramFd::set_write_callback(ReadyCallback callback, void *ctx) noexcept {
    return rwfd_.set_write_callback(callback, ctx);
}

fiber::common::IoErr DatagramFd::clear_read_callback(ReadyCallback callback, void *ctx) noexcept {
    return rwfd_.clear_read_callback(callback, ctx);
}

fiber::common::IoErr DatagramFd::clear_write_callback(ReadyCallback callback, void *ctx) noexcept {
    return rwfd_.clear_write_callback(callback, ctx);
}

fiber::common::IoErr DatagramFd::recv_packet_once(void *buf, size_t len, UdpPacketRecvResult &out) noexcept {
    int socket_fd = rwfd_.fd();
    if (socket_fd < 0) {
        return fiber::common::IoErr::BadFd;
    }

    for (;;) {
        sockaddr_storage peer{};
        iovec iov{};
        iov.iov_base = buf;
        iov.iov_len = len;
        alignas(cmsghdr) std::array<unsigned char, kRecvControlCapacity> control{};
        msghdr msg{};
        msg.msg_name = &peer;
        msg.msg_namelen = sizeof(peer);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control.data();
        msg.msg_controllen = control.size();

        ssize_t rc = ::recvmsg(socket_fd, &msg, MSG_DONTWAIT);
        if (rc >= 0) {
            SocketAddress parsed_peer;
            if (!SocketAddress::from_sockaddr(reinterpret_cast<const sockaddr *>(&peer), msg.msg_namelen,
                                              parsed_peer)) {
                return fiber::common::IoErr::NotSupported;
            }
            out.size = static_cast<size_t>(rc);
            out.peer = parsed_peer;
            out.truncated = (msg.msg_flags & MSG_TRUNC) != 0;
            parse_control_messages(msg, local_addr_, out);
            return fiber::common::IoErr::None;
        }

        int err = errno;
        if (err == EINTR) {
            continue;
        }
        if (err == EAGAIN || err == EWOULDBLOCK) {
            return fiber::common::IoErr::WouldBlock;
        }
        return fiber::common::io_err_from_errno(err);
    }
}

fiber::common::IoErr DatagramFd::send_packet_once(const UdpPacketSendSpec &spec, size_t &out) noexcept {
    out = 0;
    int socket_fd = rwfd_.fd();
    if (socket_fd < 0) {
        return fiber::common::IoErr::BadFd;
    }
    if (!send_spec_valid(spec)) {
        return fiber::common::IoErr::Invalid;
    }
    if (!send_spec_supported(spec)) {
        return fiber::common::IoErr::NotSupported;
    }

    sockaddr_storage peer_storage{};
    socklen_t peer_len = 0;
    if (!spec.peer.to_sockaddr(peer_storage, peer_len)) {
        return fiber::common::IoErr::NotSupported;
    }

    std::array<iovec, 1> single_iov{};
    const iovec *iov = spec.iov;
    int iov_count = spec.iov_count;
    if (iov_count <= 0) {
        single_iov[0].iov_base = const_cast<void *>(spec.buf);
        single_iov[0].iov_len = spec.len;
        iov = single_iov.data();
        iov_count = 1;
    }

    for (;;) {
        msghdr msg{};
        alignas(cmsghdr) std::array<unsigned char, kSendControlCapacity> control{};
        msg.msg_name = &peer_storage;
        msg.msg_namelen = peer_len;
        msg.msg_iov = const_cast<iovec *>(iov);
        msg.msg_iovlen = static_cast<std::size_t>(iov_count);
        build_send_control(spec, control, msg);

        ssize_t rc = ::sendmsg(socket_fd, &msg, MSG_DONTWAIT);
        if (rc >= 0) {
            out = static_cast<size_t>(rc);
            return fiber::common::IoErr::None;
        }

        int err = errno;
        if (err == EINTR) {
            continue;
        }
        if (err == EAGAIN || err == EWOULDBLOCK) {
            return fiber::common::IoErr::WouldBlock;
        }
        return fiber::common::io_err_from_errno(err);
    }
}

} // namespace fiber::net::detail
