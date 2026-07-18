#ifndef FIBER_NET_TCP_STREAM_H
#define FIBER_NET_TCP_STREAM_H

#include <chrono>
#include <cstddef>
#include <sys/uio.h>
#include <utility>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../event/EventLoop.h"
#include "SocketAddress.h"
#include "TcpSocketOptions.h"
#include "detail/ConnectFd.h"
#include "detail/StreamFd.h"

namespace fiber::net {

class TcpStream;

struct TcpConnectTraits {
    using Address = SocketAddress;

    static fiber::common::IoResult<int> create_socket(const Address &peer);
    static fiber::common::IoErr connect_once(int fd, const Address &peer);
};

class TcpStream : public common::NonCopyable, public common::NonMovable {
public:
    using IoTask = detail::StreamFd::IoTask;
    using ReadyCallback = detail::StreamFd::ReadyCallback;
    using WaitReadableAwaiter = detail::StreamFd::WaitReadableAwaiter;
    using WaitWritableAwaiter = detail::StreamFd::WaitWritableAwaiter;
    using ConnectAwaiter = detail::ConnectFd<TcpConnectTraits>::ConnectAwaiter;
    using ConnectInfant = detail::StreamInfant<TcpConnectTraits>;

    TcpStream(fiber::event::EventLoop &loop, int fd);
    TcpStream(fiber::event::EventLoop &loop, int fd, SocketAddress peer);
    TcpStream(ConnectInfant &&infant);
    ~TcpStream();

    // timeout bounds the asynchronous wait after connect(2) returns EINPROGRESS.
    // A non-positive timeout does not wait; milliseconds::max() waits indefinitely.
    [[nodiscard]] static ConnectAwaiter connect(fiber::event::EventLoop &loop, const SocketAddress &peer,
                                                std::chrono::milliseconds timeout) noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] fiber::event::EventLoop &loop() const noexcept;
    [[nodiscard]] const SocketAddress &remote_addr() const noexcept;
    [[nodiscard]] fiber::common::IoErr apply_socket_options(const TcpSocketOptions &options) noexcept;
    int release_fd() noexcept;
    void close();

    fiber::common::IoErr set_read_callback(ReadyCallback callback, void *ctx) noexcept;
    fiber::common::IoErr set_write_callback(ReadyCallback callback, void *ctx) noexcept;
    fiber::common::IoErr clear_read_callback(ReadyCallback callback, void *ctx) noexcept;
    fiber::common::IoErr clear_write_callback(ReadyCallback callback, void *ctx) noexcept;

    [[nodiscard]] IoTask read(void *buf, size_t len,
                              std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    [[nodiscard]] IoTask write(const void *buf, size_t len,
                               std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    [[nodiscard]] IoTask readv(const struct iovec *iov, int iovcnt,
                               std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    [[nodiscard]] IoTask writev(const struct iovec *iov, int iovcnt,
                                std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    [[nodiscard]] WaitReadableAwaiter
    wait_readable(std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    [[nodiscard]] WaitWritableAwaiter
    wait_writable(std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    [[nodiscard]] fiber::common::IoResult<size_t> try_read(void *buf, size_t len) noexcept;
    [[nodiscard]] fiber::common::IoResult<size_t> try_write(const void *buf, size_t len) noexcept;
    [[nodiscard]] fiber::common::IoResult<size_t> try_readv(const struct iovec *iov, int iovcnt) noexcept;
    [[nodiscard]] fiber::common::IoResult<size_t> try_writev(const struct iovec *iov, int iovcnt) noexcept;

private:
    detail::StreamFd stream_;
    SocketAddress remote_addr_{};
};

} // namespace fiber::net

#endif // FIBER_NET_TCP_STREAM_H
