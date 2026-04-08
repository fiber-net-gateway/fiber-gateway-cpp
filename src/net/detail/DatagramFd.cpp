#include "DatagramFd.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../../async/Timeout.h"
#include "../UdpSocket.h"

namespace fiber::net::detail {

namespace {

constexpr std::size_t kRecvControlCapacity =
        CMSG_SPACE(sizeof(in6_pktinfo)) + CMSG_SPACE(sizeof(in_pktinfo)) + CMSG_SPACE(sizeof(int));
constexpr std::size_t kSendControlCapacity =
        CMSG_SPACE(sizeof(in6_pktinfo)) + CMSG_SPACE(sizeof(in_pktinfo)) + CMSG_SPACE(sizeof(int));

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

void apply_ipv6_local(sockaddr_storage &storage, std::uint16_t port, const in6_addr &addr, std::uint32_t scope_id) noexcept {
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
    if (!spec.has_local && spec.ecn == UdpEcn::Unspecified) {
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

    msg.msg_controllen = static_cast<std::size_t>(used_end - control.data());
    return msg.msg_controllen;
}

bool send_spec_valid(const UdpPacketSendSpec &spec) noexcept {
    if (spec.iov_count > 0) {
        return spec.iov != nullptr;
    }
    return spec.len == 0 || spec.buf != nullptr;
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

DatagramFd::RecvFromAwaiter DatagramFd::recv_from(void *buf, size_t len) noexcept { return {*this, buf, len}; }

DatagramFd::SendToAwaiter DatagramFd::send_to(const void *buf, size_t len, const SocketAddress &peer) noexcept {
    return {*this, buf, len, peer};
}

DatagramFd::RecvPacketAwaiter DatagramFd::recv_packet(void *buf, size_t len) noexcept { return {*this, buf, len}; }

DatagramFd::SendPacketAwaiter DatagramFd::send_packet(UdpPacketSendSpec spec) noexcept { return {*this, spec}; }

DatagramFd::WaitEventAwaiter DatagramFd::wait_event(fiber::event::IoEvent interested) noexcept {
    return rwfd_.wait_event(interested);
}

DatagramFd::WaitReadableAwaiter DatagramFd::wait_readable() noexcept { return rwfd_.wait_readable(); }

DatagramFd::WaitWritableAwaiter DatagramFd::wait_writable() noexcept { return rwfd_.wait_writable(); }

fiber::common::IoResult<UdpRecvResult> DatagramFd::try_recv_from(void *buf, size_t len) noexcept {
    auto packet_result = try_recv_packet(buf, len);
    if (!packet_result) {
        return std::unexpected(packet_result.error());
    }
    return UdpRecvResult{packet_result->size, packet_result->peer};
}

fiber::common::IoResult<size_t> DatagramFd::try_send_to(const void *buf,
                                                        size_t len,
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

fiber::async::Task<fiber::common::IoResult<fiber::event::IoEvent>>
DatagramFd::wait_event(fiber::event::IoEvent interested, std::chrono::milliseconds timeout) noexcept {
    co_return co_await fiber::async::timeout_for([&]() { return wait_event(interested); }, timeout);
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
        std::array<unsigned char, kRecvControlCapacity> control{};
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
            if (!SocketAddress::from_sockaddr(reinterpret_cast<const sockaddr *>(&peer), msg.msg_namelen, parsed_peer)) {
                return fiber::common::IoErr::NotSupported;
            }
            out.size = static_cast<size_t>(rc);
            out.peer = parsed_peer;
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
        std::array<unsigned char, kSendControlCapacity> control{};
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

DatagramFd::RecvFromAwaiter::RecvFromAwaiter(DatagramFd &socket, void *buf, size_t len) noexcept
    : socket_(&socket), buf_(buf), len_(len) {}

DatagramFd::RecvFromAwaiter::~RecvFromAwaiter() {}

bool DatagramFd::RecvFromAwaiter::await_suspend(std::coroutine_handle<> handle) {
    err_ = fiber::common::IoErr::None;
    completed_ = false;

    err_ = socket_->recv_packet_once(buf_, len_, packet_);
    if (err_ == fiber::common::IoErr::None) {
        completed_ = true;
        return false;
    }
    if (err_ != fiber::common::IoErr::WouldBlock) {
        completed_ = true;
        return false;
    }

    waiting_ = true;
    waiter_.emplace(socket_->rwfd_);
    return waiter_->await_suspend(handle);
}

fiber::common::IoResult<UdpRecvResult> DatagramFd::RecvFromAwaiter::await_resume() noexcept {
    waiting_ = false;
    if (completed_) {
        completed_ = false;
        if (err_ == fiber::common::IoErr::None) {
            return UdpRecvResult{packet_.size, packet_.peer};
        }
        return std::unexpected(err_);
    }

    if (waiter_) {
        fiber::common::IoResult<void> wait_result = waiter_->await_resume();
        waiter_.reset();
        if (!wait_result) {
            return std::unexpected(wait_result.error());
        }
    }

    err_ = socket_->recv_packet_once(buf_, len_, packet_);
    if (err_ == fiber::common::IoErr::None) {
        return UdpRecvResult{packet_.size, packet_.peer};
    }
    return std::unexpected(err_);
}

DatagramFd::SendToAwaiter::SendToAwaiter(DatagramFd &socket, const void *buf, size_t len, SocketAddress peer) noexcept
    : socket_(&socket) {
    spec_.buf = buf;
    spec_.len = len;
    spec_.peer = std::move(peer);
}

DatagramFd::SendToAwaiter::~SendToAwaiter() {}

bool DatagramFd::SendToAwaiter::await_suspend(std::coroutine_handle<> handle) {
    err_ = fiber::common::IoErr::None;
    completed_ = false;

    err_ = socket_->send_packet_once(spec_, result_);
    if (err_ == fiber::common::IoErr::None) {
        completed_ = true;
        return false;
    }
    if (err_ != fiber::common::IoErr::WouldBlock) {
        completed_ = true;
        return false;
    }

    waiting_ = true;
    waiter_.emplace(socket_->rwfd_);
    return waiter_->await_suspend(handle);
}

fiber::common::IoResult<size_t> DatagramFd::SendToAwaiter::await_resume() noexcept {
    waiting_ = false;
    if (completed_) {
        completed_ = false;
        if (err_ == fiber::common::IoErr::None) {
            return result_;
        }
        return std::unexpected(err_);
    }

    if (waiter_) {
        fiber::common::IoResult<void> wait_result = waiter_->await_resume();
        waiter_.reset();
        if (!wait_result) {
            return std::unexpected(wait_result.error());
        }
    }

    err_ = socket_->send_packet_once(spec_, result_);
    if (err_ == fiber::common::IoErr::None) {
        return result_;
    }
    return std::unexpected(err_);
}

DatagramFd::RecvPacketAwaiter::RecvPacketAwaiter(DatagramFd &socket, void *buf, size_t len) noexcept
    : socket_(&socket), buf_(buf), len_(len) {}

DatagramFd::RecvPacketAwaiter::~RecvPacketAwaiter() {}

bool DatagramFd::RecvPacketAwaiter::await_suspend(std::coroutine_handle<> handle) {
    err_ = fiber::common::IoErr::None;
    completed_ = false;

    err_ = socket_->recv_packet_once(buf_, len_, result_);
    if (err_ == fiber::common::IoErr::None) {
        completed_ = true;
        return false;
    }
    if (err_ != fiber::common::IoErr::WouldBlock) {
        completed_ = true;
        return false;
    }

    waiting_ = true;
    waiter_.emplace(socket_->rwfd_);
    return waiter_->await_suspend(handle);
}

fiber::common::IoResult<UdpPacketRecvResult> DatagramFd::RecvPacketAwaiter::await_resume() noexcept {
    waiting_ = false;
    if (completed_) {
        completed_ = false;
        if (err_ == fiber::common::IoErr::None) {
            return result_;
        }
        return std::unexpected(err_);
    }

    if (waiter_) {
        fiber::common::IoResult<void> wait_result = waiter_->await_resume();
        waiter_.reset();
        if (!wait_result) {
            return std::unexpected(wait_result.error());
        }
    }

    err_ = socket_->recv_packet_once(buf_, len_, result_);
    if (err_ == fiber::common::IoErr::None) {
        return result_;
    }
    return std::unexpected(err_);
}

DatagramFd::SendPacketAwaiter::SendPacketAwaiter(DatagramFd &socket, UdpPacketSendSpec spec) noexcept
    : socket_(&socket), spec_(std::move(spec)) {}

DatagramFd::SendPacketAwaiter::~SendPacketAwaiter() {}

bool DatagramFd::SendPacketAwaiter::await_suspend(std::coroutine_handle<> handle) {
    err_ = fiber::common::IoErr::None;
    completed_ = false;

    err_ = socket_->send_packet_once(spec_, result_);
    if (err_ == fiber::common::IoErr::None) {
        completed_ = true;
        return false;
    }
    if (err_ != fiber::common::IoErr::WouldBlock) {
        completed_ = true;
        return false;
    }

    waiting_ = true;
    waiter_.emplace(socket_->rwfd_);
    return waiter_->await_suspend(handle);
}

fiber::common::IoResult<size_t> DatagramFd::SendPacketAwaiter::await_resume() noexcept {
    waiting_ = false;
    if (completed_) {
        completed_ = false;
        if (err_ == fiber::common::IoErr::None) {
            return result_;
        }
        return std::unexpected(err_);
    }

    if (waiter_) {
        fiber::common::IoResult<void> wait_result = waiter_->await_resume();
        waiter_.reset();
        if (!wait_result) {
            return std::unexpected(wait_result.error());
        }
    }

    err_ = socket_->send_packet_once(spec_, result_);
    if (err_ == fiber::common::IoErr::None) {
        return result_;
    }
    return std::unexpected(err_);
}

} // namespace fiber::net::detail
