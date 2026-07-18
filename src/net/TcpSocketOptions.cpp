#include "TcpSocketOptions.h"

#include <cerrno>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

namespace fiber::net::detail {

common::IoErr apply_tcp_socket_options(int fd, const TcpSocketOptions &options) noexcept {
    if (fd < 0) {
        return common::IoErr::BadFd;
    }
    int no_delay = 0;
    switch (options.no_delay) {
        case TcpOptionMode::Unchanged:
            return common::IoErr::None;
        case TcpOptionMode::Enabled:
            no_delay = 1;
            break;
        case TcpOptionMode::Disabled:
            break;
        default:
            return common::IoErr::Invalid;
    }
    if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay)) != 0) {
        return common::io_err_from_errno(errno);
    }
    return common::IoErr::None;
}

} // namespace fiber::net::detail
