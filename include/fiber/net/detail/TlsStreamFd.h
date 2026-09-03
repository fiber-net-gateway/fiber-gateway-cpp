#ifndef FIBER_NET_DETAIL_TLS_STREAM_FD_H
#define FIBER_NET_DETAIL_TLS_STREAM_FD_H

#include <chrono>
#include <cstddef>
#include <string_view>

#include "../../async/Task.h"
#include "../../common/IoError.h"
#include "../../common/NonCopyable.h"
#include "../../common/NonMovable.h"
#include "../../event/Poller.h"
#include "../TlsParams.h"
#include "StreamFd.h"
#include "TlsHandshakeState.h"

struct ssl_st;
typedef struct ssl_st SSL;

namespace fiber::net::detail {

class TlsStreamFd : public common::NonCopyable, public common::NonMovable {
public:
    using HandshakeTask = fiber::async::Task<fiber::common::IoResult<void>>;
    using ShutdownTask = fiber::async::Task<fiber::common::IoResult<void>>;
    using IoTask = fiber::async::Task<fiber::common::IoResult<size_t>>;
    using ReadyCallback = StreamFd::ReadyCallback;

    TlsStreamFd(fiber::event::EventLoop &loop, int fd);
    ~TlsStreamFd();

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] fiber::event::EventLoop &loop() const noexcept;
    // Borrowed view into ssl_. Invalidated by close() or destruction.
    [[nodiscard]] std::string_view selected_alpn() const noexcept;
    [[nodiscard]] bool handshake_done() const noexcept;
    [[nodiscard]] bool has_pending_read() const noexcept;
    [[nodiscard]] bool terminal() const noexcept { return stream_fd_.terminal(); }
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
    [[nodiscard]] StreamFd::WaitReadableAwaiter
    wait_readable(std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    [[nodiscard]] StreamFd::WaitWritableAwaiter
    wait_writable(std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    fiber::common::IoErr poll_shutdown(fiber::event::IoEvent &event) noexcept;
    fiber::common::IoErr poll_read(void *buf, size_t len, size_t &out, fiber::event::IoEvent &event) noexcept;
    fiber::common::IoErr poll_write(const void *buf, size_t len, size_t &out, fiber::event::IoEvent &event) noexcept;

private:
    enum class Role : std::uint8_t {
        None,
        Client,
        Server,
    };

    common::IoResult<void> start_client(const TlsClientParam &param) noexcept;
    common::IoResult<void> start_server(const TlsServerParam &param) noexcept;
    common::IoResult<void> attach_ssl(SSL *ssl) noexcept;
    // server_param is the caller's param, borrowed for the handshake's
    // duration; the server handshake state lives in the coroutine frame.
    [[nodiscard]] HandshakeTask handshake_impl(common::IoResult<void> start_result, std::chrono::milliseconds timeout,
                                               const TlsServerParam *server_param);
    fiber::common::IoErr handshake_once(fiber::event::IoEvent &event) noexcept;
    fiber::common::IoErr shutdown_once(fiber::event::IoEvent &event) noexcept;
    fiber::common::IoErr read_once(void *buf, size_t len, size_t &out, fiber::event::IoEvent &event) noexcept;
    fiber::common::IoErr write_once(const void *buf, size_t len, size_t &out, fiber::event::IoEvent &event) noexcept;
    StreamFd stream_fd_;
    SSL *ssl_ = nullptr;
    Role role_ = Role::None;
    bool handshake_done_ = false;
    bool busy_ = false;
};

} // namespace fiber::net::detail

#endif // FIBER_NET_DETAIL_TLS_STREAM_FD_H
