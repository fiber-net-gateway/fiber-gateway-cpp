#ifndef FIBER_NET_TCP_CONNECTOR_H
#define FIBER_NET_TCP_CONNECTOR_H

#include <expected>
#include <span>

#include "HappyEyeballs.h"
#include "TcpStream.h"
#include "detail/HappyEyeballsConnectFd.h"

namespace fiber::net {

// RFC 8305-style connection-phase racing for an already-resolved, bounded address set. DNS and
// destination sorting remain caller responsibilities. The existing single-address
// TcpStream::connect API is unchanged.
class TcpConnector {
public:
    using ConnectAwaiter = detail::HappyEyeballsConnectFd<TcpConnectTraits>::ConnectAwaiter;
    using ConnectResult = detail::HappyEyeballsConnectFd<TcpConnectTraits>::ConnectResult;

    [[nodiscard]] static ConnectAwaiter connect(event::EventLoop &loop, std::span<const SocketAddress> addresses,
                                                HappyEyeballsOptions options = {}) noexcept {
        return detail::HappyEyeballsConnectFd<TcpConnectTraits>::connect(loop, addresses, options);
    }
};

} // namespace fiber::net

#endif // FIBER_NET_TCP_CONNECTOR_H
