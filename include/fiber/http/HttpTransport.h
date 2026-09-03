#ifndef FIBER_HTTP_HTTP_TRANSPORT_H
#define FIBER_HTTP_HTTP_TRANSPORT_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../common/mem/IoBufChain.h"
#include "../event/EventLoop.h"
#include "../net/TcpListener.h"
#include "../net/TcpStream.h"
#include "../net/TlsParams.h"
#include "../net/TlsTcpStream.h"

namespace fiber::http {

class HttpTransport : public common::NonCopyable, public common::NonMovable {
public:
    using ReadyCallback = void (*)(void *ctx, common::IoErr err) noexcept;

    virtual ~HttpTransport() = default;

    virtual fiber::async::Task<common::IoResult<void>> handshake(std::chrono::milliseconds timeout) = 0;
    virtual fiber::async::Task<common::IoResult<void>> shutdown(std::chrono::milliseconds timeout) = 0;
    virtual fiber::async::Task<common::IoResult<void>> wait_readable(std::chrono::milliseconds timeout) = 0;
    [[nodiscard]] virtual bool has_pending_read() const noexcept { return false; }
    // Some stateful transports require a WouldBlock read to be retried with
    // the same destination buffer and length.
    [[nodiscard]] virtual bool requires_stable_read_buffer_on_retry() const noexcept { return false; }

    // Readiness callbacks are persistent and run on loop(). close() completes
    // registered callbacks with Canceled. Callers may update registration from
    // a callback but must keep the transport alive until dispatch returns. They
    // share readiness slots with waiters, so the same physical direction cannot
    // use callback and awaitable modes at the same time.
    virtual common::IoErr set_read_callback(ReadyCallback callback, void *ctx) noexcept = 0;
    virtual common::IoErr set_write_callback(ReadyCallback callback, void *ctx) noexcept = 0;
    virtual common::IoErr set_terminal_callback(ReadyCallback callback, void *ctx) noexcept = 0;
    virtual common::IoErr clear_read_callback(ReadyCallback callback, void *ctx) noexcept = 0;
    virtual common::IoErr clear_write_callback(ReadyCallback callback, void *ctx) noexcept = 0;
    virtual common::IoErr clear_terminal_callback(ReadyCallback callback, void *ctx) noexcept = 0;

    // poll_* performs one non-suspending transport operation. wait_event is set
    // only when WouldBlock is returned. For TLS it may be the opposite physical
    // direction from the logical operation. Buffers passed to a TLS operation
    // must remain unchanged until that operation succeeds, fails, or the
    // transport is closed.
    virtual common::IoErr poll_read(void *buf, size_t len, size_t &out, event::IoEvent &wait_event) noexcept = 0;
    virtual common::IoErr poll_read_into(mem::IoBuf &buf, size_t &out, event::IoEvent &wait_event) noexcept = 0;
    virtual common::IoErr poll_readv_into(mem::IoBufChain &bufs, size_t &out, event::IoEvent &wait_event) noexcept = 0;
    virtual common::IoErr poll_write(const void *buf, size_t len, size_t &out, event::IoEvent &wait_event) noexcept = 0;
    virtual common::IoErr poll_write(mem::IoBuf &buf, size_t &out, event::IoEvent &wait_event) noexcept = 0;
    virtual common::IoErr poll_writev(mem::IoBufChain &buf, size_t &out, event::IoEvent &wait_event) noexcept = 0;

    virtual fiber::async::Task<common::IoResult<size_t>> read(void *buf, size_t len,
                                                              std::chrono::milliseconds timeout) = 0;
    virtual fiber::async::Task<common::IoResult<size_t>> read_into(mem::IoBuf &buf,
                                                                   std::chrono::milliseconds timeout) = 0;
    virtual fiber::async::Task<common::IoResult<size_t>> readv_into(mem::IoBufChain &bufs,
                                                                    std::chrono::milliseconds timeout) = 0;
    virtual fiber::async::Task<common::IoResult<size_t>> write(const void *buf, size_t len,
                                                               std::chrono::milliseconds timeout) = 0;
    virtual fiber::async::Task<common::IoResult<size_t>> write(mem::IoBuf &buf, std::chrono::milliseconds timeout) = 0;
    virtual fiber::async::Task<common::IoResult<size_t>> writev(mem::IoBufChain &buf,
                                                                std::chrono::milliseconds timeout) = 0;
    // Drops transport-owned references to buffers from an abandoned operation.
    // All active I/O tasks must be canceled first. This does not close the fd.
    virtual void abandon_pending_io() noexcept {}
    virtual void close() = 0;
    [[nodiscard]] virtual bool valid() const noexcept = 0;
    [[nodiscard]] virtual bool terminal() const noexcept = 0;
    [[nodiscard]] virtual int fd() const noexcept = 0;
    // Borrowed view into the transport. Invalidated by close() or destruction.
    [[nodiscard]] virtual std::string_view negotiated_alpn() const noexcept = 0;
    [[nodiscard]] virtual const net::SocketAddress &remote_addr() const noexcept = 0;
    [[nodiscard]] virtual event::EventLoop &loop() const noexcept = 0;
};

class TcpTransport final : public HttpTransport {
public:
    static common::IoResult<std::unique_ptr<TcpTransport>> create(event::EventLoop &loop, net::AcceptResult &&accept,
                                                                  net::TcpSocketOptions tcp_options = {});

    fiber::async::Task<common::IoResult<void>> handshake(std::chrono::milliseconds timeout) override;
    fiber::async::Task<common::IoResult<void>> shutdown(std::chrono::milliseconds timeout) override;
    fiber::async::Task<common::IoResult<void>> wait_readable(std::chrono::milliseconds timeout) override;
    [[nodiscard]] bool has_pending_read() const noexcept override { return false; }
    common::IoErr set_read_callback(ReadyCallback callback, void *ctx) noexcept override;
    common::IoErr set_write_callback(ReadyCallback callback, void *ctx) noexcept override;
    common::IoErr set_terminal_callback(ReadyCallback callback, void *ctx) noexcept override;
    common::IoErr clear_read_callback(ReadyCallback callback, void *ctx) noexcept override;
    common::IoErr clear_write_callback(ReadyCallback callback, void *ctx) noexcept override;
    common::IoErr clear_terminal_callback(ReadyCallback callback, void *ctx) noexcept override;
    common::IoErr poll_read(void *buf, size_t len, size_t &out, event::IoEvent &wait_event) noexcept override;
    common::IoErr poll_read_into(mem::IoBuf &buf, size_t &out, event::IoEvent &wait_event) noexcept override;
    common::IoErr poll_readv_into(mem::IoBufChain &bufs, size_t &out, event::IoEvent &wait_event) noexcept override;
    common::IoErr poll_write(const void *buf, size_t len, size_t &out, event::IoEvent &wait_event) noexcept override;
    common::IoErr poll_write(mem::IoBuf &buf, size_t &out, event::IoEvent &wait_event) noexcept override;
    common::IoErr poll_writev(mem::IoBufChain &buf, size_t &out, event::IoEvent &wait_event) noexcept override;
    fiber::async::Task<common::IoResult<size_t>> read(void *buf, size_t len,
                                                      std::chrono::milliseconds timeout) override;
    fiber::async::Task<common::IoResult<size_t>> read_into(mem::IoBuf &buf, std::chrono::milliseconds timeout) override;
    fiber::async::Task<common::IoResult<size_t>> readv_into(mem::IoBufChain &bufs,
                                                            std::chrono::milliseconds timeout) override;
    fiber::async::Task<common::IoResult<size_t>> write(const void *buf, size_t len,
                                                       std::chrono::milliseconds timeout) override;
    fiber::async::Task<common::IoResult<size_t>> write(mem::IoBuf &buf, std::chrono::milliseconds timeout) override;
    fiber::async::Task<common::IoResult<size_t>> writev(mem::IoBufChain &buf,
                                                        std::chrono::milliseconds timeout) override;
    void close() override;
    [[nodiscard]] bool valid() const noexcept override;
    [[nodiscard]] bool terminal() const noexcept override;
    [[nodiscard]] int fd() const noexcept override;
    [[nodiscard]] std::string_view negotiated_alpn() const noexcept override;
    [[nodiscard]] const net::SocketAddress &remote_addr() const noexcept override;
    [[nodiscard]] event::EventLoop &loop() const noexcept override;

private:
    TcpTransport(event::EventLoop &loop, int fd, net::SocketAddress remote_addr);

    net::TcpStream stream_;
};

class TlsTransport final : public HttpTransport {
public:
    ~TlsTransport() override;
    static common::IoResult<std::unique_ptr<TlsTransport>> create(event::EventLoop &loop, net::AcceptResult &&accept,
                                                                  net::TlsClientParam options,
                                                                  net::TcpSocketOptions tcp_options = {});
    static common::IoResult<std::unique_ptr<TlsTransport>> create(event::EventLoop &loop, net::AcceptResult &&accept,
                                                                  const net::TlsServerParam &options,
                                                                  net::TcpSocketOptions tcp_options = {});

    fiber::async::Task<common::IoResult<void>> handshake(std::chrono::milliseconds timeout) override;
    fiber::async::Task<common::IoResult<void>> shutdown(std::chrono::milliseconds timeout) override;
    fiber::async::Task<common::IoResult<void>> wait_readable(std::chrono::milliseconds timeout) override;
    [[nodiscard]] bool has_pending_read() const noexcept override;
    [[nodiscard]] bool requires_stable_read_buffer_on_retry() const noexcept override { return true; }
    common::IoErr set_read_callback(ReadyCallback callback, void *ctx) noexcept override;
    common::IoErr set_write_callback(ReadyCallback callback, void *ctx) noexcept override;
    common::IoErr set_terminal_callback(ReadyCallback callback, void *ctx) noexcept override;
    common::IoErr clear_read_callback(ReadyCallback callback, void *ctx) noexcept override;
    common::IoErr clear_write_callback(ReadyCallback callback, void *ctx) noexcept override;
    common::IoErr clear_terminal_callback(ReadyCallback callback, void *ctx) noexcept override;
    common::IoErr poll_read(void *buf, size_t len, size_t &out, event::IoEvent &wait_event) noexcept override;
    common::IoErr poll_read_into(mem::IoBuf &buf, size_t &out, event::IoEvent &wait_event) noexcept override;
    common::IoErr poll_readv_into(mem::IoBufChain &bufs, size_t &out, event::IoEvent &wait_event) noexcept override;
    common::IoErr poll_write(const void *buf, size_t len, size_t &out, event::IoEvent &wait_event) noexcept override;
    common::IoErr poll_write(mem::IoBuf &buf, size_t &out, event::IoEvent &wait_event) noexcept override;
    common::IoErr poll_writev(mem::IoBufChain &buf, size_t &out, event::IoEvent &wait_event) noexcept override;
    fiber::async::Task<common::IoResult<size_t>> read(void *buf, size_t len,
                                                      std::chrono::milliseconds timeout) override;
    fiber::async::Task<common::IoResult<size_t>> read_into(mem::IoBuf &buf, std::chrono::milliseconds timeout) override;
    fiber::async::Task<common::IoResult<size_t>> readv_into(mem::IoBufChain &bufs,
                                                            std::chrono::milliseconds timeout) override;
    fiber::async::Task<common::IoResult<size_t>> write(const void *buf, size_t len,
                                                       std::chrono::milliseconds timeout) override;
    fiber::async::Task<common::IoResult<size_t>> write(mem::IoBuf &buf, std::chrono::milliseconds timeout) override;
    fiber::async::Task<common::IoResult<size_t>> writev(mem::IoBufChain &buf,
                                                        std::chrono::milliseconds timeout) override;
    void abandon_pending_io() noexcept override;
    void close() override;
    [[nodiscard]] bool valid() const noexcept override;
    [[nodiscard]] bool terminal() const noexcept override;
    [[nodiscard]] int fd() const noexcept override;
    [[nodiscard]] std::string_view negotiated_alpn() const noexcept override;
    [[nodiscard]] const net::SocketAddress &remote_addr() const noexcept override;
    [[nodiscard]] event::EventLoop &loop() const noexcept override;

private:
    TlsTransport(event::EventLoop &loop, int fd, net::SocketAddress remote_addr, net::TlsClientParam options);
    TlsTransport(event::EventLoop &loop, int fd, net::SocketAddress remote_addr, const net::TlsServerParam &options);
    [[nodiscard]] bool handshake_done() const noexcept;
    void clear_pending_write() noexcept;

    enum class PendingWriteKind {
        None,
        Contiguous,
        Chain,
    };

    net::TlsTcpStream stream_;
    std::optional<net::TlsClientParam> client_options_{};
    const net::TlsServerParam *server_options_ = nullptr;
    std::unique_ptr<std::uint8_t[]> writev_scratch_;
    PendingWriteKind pending_write_kind_ = PendingWriteKind::None;
    const void *pending_write_data_ = nullptr;
    std::size_t pending_write_len_ = 0;
    mem::IoBufChain *pending_write_chain_ = nullptr;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP_TRANSPORT_H
