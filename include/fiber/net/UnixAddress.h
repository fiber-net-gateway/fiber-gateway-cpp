#ifndef FIBER_NET_UNIX_ADDRESS_H
#define FIBER_NET_UNIX_ADDRESS_H

#include <string>
#include <sys/socket.h>

namespace fiber::net {

enum class UnixAddressKind {
    Filesystem,
    Abstract,
    Unnamed,
};

class UnixAddress {
public:
    static UnixAddress filesystem(std::string path);
    static UnixAddress abstract(std::string bytes);
    static UnixAddress unnamed();

    [[nodiscard]] UnixAddressKind kind() const noexcept;
    [[nodiscard]] const std::string &path() const noexcept;
    [[nodiscard]] const std::string &bytes() const noexcept;

    bool to_sockaddr(sockaddr_storage &out, socklen_t &len) const noexcept;
    static bool from_sockaddr(const sockaddr *addr, socklen_t len, UnixAddress &out) noexcept;
    std::string to_string() const;

private:
    UnixAddress(UnixAddressKind kind, std::string value);

    UnixAddressKind kind_ = UnixAddressKind::Unnamed;
    std::string value_{};
};

} // namespace fiber::net

#endif // FIBER_NET_UNIX_ADDRESS_H
