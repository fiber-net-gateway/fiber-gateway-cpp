#ifndef FIBER_NET_SOCKET_ADDRESS_H
#define FIBER_NET_SOCKET_ADDRESS_H

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <sys/socket.h>

#include "IpAddress.h"

namespace fiber::net {

class SocketAddress {
public:
    // Worst case "[<v6 text>]:<port>": brackets+colon (3) and port digits (5) on top of the ip
    // text (IpAddress::kMaxTextLen already includes NUL room).
    static constexpr std::size_t kMaxTextLen = IpAddress::kMaxTextLen + 3 + 5;

    SocketAddress();
    SocketAddress(IpAddress ip, std::uint16_t port);

    static SocketAddress any_v4(std::uint16_t port = 0);
    static SocketAddress any_v6(std::uint16_t port = 0);

    [[nodiscard]] const IpAddress &ip() const noexcept { return ip_; }
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    [[nodiscard]] IpFamily family() const noexcept { return ip_.family(); }

    std::string to_string() const;
    // Zero-allocation variant; the returned view borrows `out` (which stays NUL-terminated).
    // An unspecified ip family yields an empty view.
    [[nodiscard]] std::string_view to_string_view(std::array<char, kMaxTextLen> &out) const noexcept;

    bool to_sockaddr(sockaddr_storage &out, socklen_t &len) const noexcept;
    static bool from_sockaddr(const sockaddr *addr, socklen_t len, SocketAddress &out);

private:
    IpAddress ip_{};
    std::uint16_t port_{0};
};

} // namespace fiber::net

#endif // FIBER_NET_SOCKET_ADDRESS_H
