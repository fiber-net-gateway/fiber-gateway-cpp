#ifndef FIBER_NET_TLS_TCP_STREAM_H
#define FIBER_NET_TLS_TCP_STREAM_H

#include <chrono>
#include <cstddef>
#include <string_view>
#include <utility>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../event/EventLoop.h"
#include "../event/Poller.h"
#include "SocketAddress.h"
#include "TcpSocketOptions.h"
#include "TlsParams.h"
#include "detail/TlsStreamFd.h"

namespace fiber::net {

class TlsTcpStream : public common::NonCopyable, public common::NonMovable {
public:
    using IoTask = detail::TlsStreamFd::IoTask;
    using HandshakeTask = detail::TlsStreamFd::HandshakeTask;
    using ShutdownTask = detail::TlsStreamFd::ShutdownTask;
    using ReadyCallback = detail::TlsStreamFd::ReadyCallback;

    TlsTcpStream(fiber::event::EventLoop &loop, int fd, SocketAddress remote_addr);
    ~TlsTcpStream();

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] fiber::event::EventLoop &loop() const noexcept;
    [[nodiscard]] const SocketAddress &remote_addr() const noexcept;
    [[nodiscard]] std::string_view selected_alpn() const noexcept;
    [[nodiscard]] bool handshake_done() const noexcept;
    [[nodiscard]] bool has_pending_read() const noexcept;
    [[nodiscard]] bool terminal() const noexcept;
    [[nodiscard]] fiber::common::IoErr apply_socket_options(const TcpSocketOptions &options) noexcept;
    void close();

    fiber::common::IoErr set_read_callback(ReadyCallback callback, void *ctx) noexcept;
    fiber::common::IoErr set_write_callback(ReadyCallback callback, void *ctx) noexcept;
    fiber::common::IoErr set_terminal_callback(ReadyCallback callback, void *ctx) noexcept;
    fiber::common::IoErr clear_read_callback(ReadyCallback callback, void *ctx) noexcept;
    fiber::common::IoErr clear_write_callback(ReadyCallback callback, void *ctx) noexcept;
    fiber::common::IoErr clear_terminal_callback(ReadyCallback callback, void *ctx) noexcept;

    [[nodiscard]] IoTask read(void *buf, size_t len,
                              std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    [[nodiscard]] IoTask write(const void *buf, size_t len,
                               std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    [[nodiscard]] fiber::common::IoResult<size_t> try_read(void *buf, size_t len) noexcept;
    [[nodiscard]] fiber::common::IoResult<size_t> try_write(const void *buf, size_t len) noexcept;
    [[nodiscard]] HandshakeTask handshake(const TlsClientParam &param,
                                          std::chrono::milliseconds timeout = kDefaultTlsHandshakeTimeout);
    [[nodiscard]] HandshakeTask handshake(const TlsServerParam &param,
                                          std::chrono::milliseconds timeout = kDefaultTlsHandshakeTimeout);
    [[nodiscard]] ShutdownTask shutdown();
    [[nodiscard]] detail::StreamFd::WaitReadableAwaiter
    wait_readable(std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    [[nodiscard]] detail::StreamFd::WaitWritableAwaiter
    wait_writable(std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    fiber::common::IoErr poll_shutdown(fiber::event::IoEvent &event) noexcept;
    fiber::common::IoErr poll_read(void *buf, size_t len, size_t &out, fiber::event::IoEvent &event) noexcept;
    fiber::common::IoErr poll_write(const void *buf, size_t len, size_t &out, fiber::event::IoEvent &event) noexcept;

private:
    detail::TlsStreamFd stream_;
    SocketAddress remote_addr_{};
};

} // namespace fiber::net

#endif // FIBER_NET_TLS_TCP_STREAM_H
