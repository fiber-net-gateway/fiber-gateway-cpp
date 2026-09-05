#ifndef FIBER_NET_TCP_SOCKET_OPTIONS_H
#define FIBER_NET_TCP_SOCKET_OPTIONS_H

#include <cstdint>

#include "../common/IoError.h"

namespace fiber::net {

enum class TcpOptionMode : std::uint8_t {
    Unchanged,
    Enabled,
    Disabled,
};

struct TcpSocketOptions {
    TcpOptionMode no_delay = TcpOptionMode::Unchanged;
};

// Default for client connections that write small request headers: Nagle would
// hold a request back waiting for more bytes that never come.
inline constexpr TcpSocketOptions kNoDelayTcpSocketOptions{.no_delay = TcpOptionMode::Enabled};

namespace detail {

[[nodiscard]] common::IoErr apply_tcp_socket_options(int fd, const TcpSocketOptions &options) noexcept;

} // namespace detail

} // namespace fiber::net

#endif // FIBER_NET_TCP_SOCKET_OPTIONS_H
