#include "SocketAddress.h"

#include <arpa/inet.h>
#include <cstring>

namespace fiber::net {

SocketAddress::SocketAddress() : ip_(IpAddress::any_v4()), port_(0) {}

SocketAddress::SocketAddress(IpAddress ip, std::uint16_t port) : ip_(ip), port_(port) {}

SocketAddress SocketAddress::any_v4(std::uint16_t port) {
    return {IpAddress::any_v4(), port};
}

SocketAddress SocketAddress::any_v6(std::uint16_t port) {
    return {IpAddress::any_v6(), port};
}

std::string SocketAddress::to_string() const {
    std::string ip_text = ip_.to_string();
    if (ip_text.empty()) {
        return {};
    }
    if (ip_.is_v6()) {
        return "[" + ip_text + "]:" + std::to_string(port_);
    }
    return ip_text + ":" + std::to_string(port_);
}

bool SocketAddress::to_sockaddr(sockaddr_storage &out, socklen_t &len) const noexcept {
    if (ip_.is_v4()) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);
        std::memcpy(&addr.sin_addr, ip_.v4_bytes().data(), ip_.v4_bytes().size());
        std::memset(&out, 0, sizeof(out));
        std::memcpy(&out, &addr, sizeof(addr));
        len = static_cast<socklen_t>(sizeof(addr));
        return true;
    }
    if (ip_.is_v6()) {
        sockaddr_in6 addr{};
        addr.sin6_family = AF_INET6;
        addr.sin6_port = htons(port_);
        addr.sin6_scope_id = ip_.scope_id();
        std::memcpy(&addr.sin6_addr, ip_.v6_bytes().data(), ip_.v6_bytes().size());
        std::memset(&out, 0, sizeof(out));
        std::memcpy(&out, &addr, sizeof(addr));
        len = static_cast<socklen_t>(sizeof(addr));
        return true;
    }
    return false;
}

bool SocketAddress::from_sockaddr(const sockaddr *addr, socklen_t len, SocketAddress &out) {
    if (!addr) {
        return false;
    }
    if (addr->sa_family == AF_INET) {
        if (len < static_cast<socklen_t>(sizeof(sockaddr_in))) {
            return false;
        }
        const auto *in = reinterpret_cast<const sockaddr_in *>(addr);
        std::array<std::uint8_t, 4> bytes{};
        std::memcpy(bytes.data(), &in->sin_addr, bytes.size());
        out = SocketAddress(IpAddress::v4(bytes), ntohs(in->sin_port));
        return true;
    }
    if (addr->sa_family == AF_INET6) {
        if (len < static_cast<socklen_t>(sizeof(sockaddr_in6))) {
            return false;
        }
        const auto *in6 = reinterpret_cast<const sockaddr_in6 *>(addr);
        std::array<std::uint8_t, 16> bytes{};
        std::memcpy(bytes.data(), &in6->sin6_addr, bytes.size());
        out = SocketAddress(IpAddress::v6(bytes, in6->sin6_scope_id), ntohs(in6->sin6_port));
        return true;
    }
    return false;
}

} // namespace fiber::net
