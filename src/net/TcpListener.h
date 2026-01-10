#ifndef FIBER_NET_TCP_LISTENER_H
#define FIBER_NET_TCP_LISTENER_H

#include <cstdint>
#include <sys/socket.h>

#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../common/IoError.h"
#include "../event/EventLoop.h"
#include "detail/AcceptFd.h"
#include "SocketAddress.h"

namespace fiber::net {

struct ListenOptions {
    int backlog = SOMAXCONN;
    bool reuse_addr = true;
    bool reuse_port = false;
    bool v6_only = false;
};

struct AcceptResult {
    int fd = -1;
    SocketAddress peer{};
};

struct TcpTraits {
    using Address = SocketAddress;
    using ListenOptions = fiber::net::ListenOptions;
    using AcceptResult = fiber::net::AcceptResult;

    static fiber::common::IoResult<int> bind(const Address &addr,
                                             const ListenOptions &options);
    static fiber::common::IoErr accept_once(int fd, AcceptResult &out);
};

class TcpListener : public common::NonCopyable, public common::NonMovable {
public:
    using AcceptAwaiter = detail::AcceptFd<TcpTraits>::AcceptAwaiter;

    explicit TcpListener(fiber::event::EventLoop &loop);
    ~TcpListener();

    fiber::common::IoResult<void> bind(const SocketAddress &addr,
                                       const ListenOptions &options);
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] int fd() const noexcept;
    void close();

    [[nodiscard]] AcceptAwaiter accept() noexcept;

private:
    detail::AcceptFd<TcpTraits> acceptor_;
};

} // namespace fiber::net

#endif // FIBER_NET_TCP_LISTENER_H
