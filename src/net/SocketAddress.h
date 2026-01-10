#ifndef FIBER_NET_SOCKET_ADDRESS_H
#define FIBER_NET_SOCKET_ADDRESS_H

#include <cstdint>
#include <string>
#include <sys/socket.h>

#include "IpAddress.h"

namespace fiber::net {

class SocketAddress {
public:
    SocketAddress();
    SocketAddress(IpAddress ip, std::uint16_t port);

    static SocketAddress any_v4(std::uint16_t port = 0);
    static SocketAddress any_v6(std::uint16_t port = 0);

    [[nodiscard]] const IpAddress &ip() const noexcept {
        return ip_;
    }
    [[nodiscard]] std::uint16_t port() const noexcept {
        return port_;
    }
    [[nodiscard]] IpFamily family() const noexcept {
        return ip_.family();
    }

    std::string to_string() const;

    bool to_sockaddr(sockaddr_storage &out, socklen_t &len) const noexcept;
    static bool from_sockaddr(const sockaddr *addr, socklen_t len, SocketAddress &out);

private:
    IpAddress ip_{};
    std::uint16_t port_{0};
};

} // namespace fiber::net

#endif // FIBER_NET_SOCKET_ADDRESS_H
