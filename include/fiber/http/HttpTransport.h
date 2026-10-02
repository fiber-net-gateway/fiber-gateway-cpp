#ifndef FIBER_HTTP_HTTP_TRANSPORT_H
#define FIBER_HTTP_HTTP_TRANSPORT_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
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

    virtual fiber::async::Task<common::IoResult<void>> shutdown(std::chrono::milliseconds timeout) = 0;
    virtual fiber::async::Task<common::IoResult<void>> wait_readable(std::chrono::milliseconds timeout) = 0;

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

    // Direction readiness. A Ready direction must be advanced by doing I/O;
    // installing a readiness subscription for it violates the subscription
    // contract. Read readiness includes what no fd edge announces (TLS
    // plaintext opened from a wire read already consumed), so a reader
    // drains until WouldBlock or until this turns false. Write readiness
    // excludes accepted bytes still held (TLS sealed records the socket has
    // not taken): Ready implies everything accepted is on the wire.
    [[nodiscard]] virtual bool read_ready() const noexcept = 0;
    [[nodiscard]] virtual bool write_ready() const noexcept = 0;

    // Loop handover, see net::detail::RWFd. detach runs on the current loop
    // and requires no active I/O, subscriptions or pending buffers; adopt runs
    // on the target loop thread.
    virtual common::IoErr detach_for_handover() noexcept = 0;
    virtual common::IoErr adopt_loop(event::EventLoop &loop) noexcept = 0;

    // Idle-pool observation: ensure the fd listens for the stream-state bits
    // (peer hangup) without subscribing a direction callback, so a parked
    // connection whose peer went away is detected instead of handed out.
    [[nodiscard]] virtual common::IoErr ensure_state_observation() noexcept { return common::IoErr::None; }

    // Reads are append-only: try_readv/readv append freshly allocated nodes
    // holding at most `size` bytes to `out` (one non-suspending transport
    // operation; `size` is a cap, not a target). WouldBlock leaves the chain
    // untouched. Returns 0 on EOF.
    [[nodiscard]] virtual common::IoResult<size_t> try_readv(size_t size, mem::IoBufChain &out) noexcept = 0;
    virtual fiber::async::Task<common::IoResult<size_t>> readv(size_t size, mem::IoBufChain &out,
                                                               std::chrono::milliseconds timeout) = 0;

    // Writes consume from the front of `buf` and may complete partially; loop
    // until readable_bytes() == 0. No transport keeps a reference to the
    // chain past the call. Bytes try_writev reports may still sit in the
    // transport (TLS: sealed records the socket has not taken); they drain on
    // their own, and the write direction turns ready only once they have.
    // close() drops them, so a graceful closer waits for write readiness
    // first. writev returns once its bytes are on the wire.
    [[nodiscard]] virtual common::IoResult<size_t> try_writev(mem::IoBufChain &buf) noexcept = 0;
    virtual fiber::async::Task<common::IoResult<size_t>> writev(mem::IoBufChain &buf,
                                                                std::chrono::milliseconds timeout) = 0;
    virtual void close() = 0;
    [[nodiscard]] virtual bool valid() const noexcept = 0;
    [[nodiscard]] virtual bool terminal() const noexcept = 0;
    // True once the peer closed its write side or the connection terminated;
    // a pooled connection must not be reused when this is set.
    [[nodiscard]] virtual bool peer_closed() const noexcept { return terminal(); }
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

    fiber::async::Task<common::IoResult<void>> shutdown(std::chrono::milliseconds timeout) override;
    fiber::async::Task<common::IoResult<void>> wait_readable(std::chrono::milliseconds timeout) override;
    common::IoErr set_read_callback(ReadyCallback callback, void *ctx) noexcept override;
    common::IoErr set_write_callback(ReadyCallback callback, void *ctx) noexcept override;
    common::IoErr set_terminal_callback(ReadyCallback callback, void *ctx) noexcept override;
    common::IoErr clear_read_callback(ReadyCallback callback, void *ctx) noexcept override;
    common::IoErr clear_write_callback(ReadyCallback callback, void *ctx) noexcept override;
    common::IoErr clear_terminal_callback(ReadyCallback callback, void *ctx) noexcept override;
    [[nodiscard]] bool read_ready() const noexcept override;
    [[nodiscard]] bool write_ready() const noexcept override;
    common::IoErr detach_for_handover() noexcept override;
    common::IoErr adopt_loop(event::EventLoop &loop) noexcept override;
    [[nodiscard]] common::IoErr ensure_state_observation() noexcept override;
    [[nodiscard]] common::IoResult<size_t> try_readv(size_t size, mem::IoBufChain &out) noexcept override;
    fiber::async::Task<common::IoResult<size_t>> readv(size_t size, mem::IoBufChain &out,
                                                       std::chrono::milliseconds timeout) override;
    [[nodiscard]] common::IoResult<size_t> try_writev(mem::IoBufChain &buf) noexcept override;
    fiber::async::Task<common::IoResult<size_t>> writev(mem::IoBufChain &buf,
                                                        std::chrono::milliseconds timeout) override;
    void close() override;
    [[nodiscard]] bool valid() const noexcept override;
    [[nodiscard]] bool terminal() const noexcept override;
    [[nodiscard]] bool peer_closed() const noexcept override;
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
                                                                  net::TcpSocketOptions tcp_options = {});

    // The param is borrowed only for the duration of the handshake (see
    // net::TlsServerParam/TlsClientParam); TlsTransport does not retain it.
    fiber::async::Task<common::IoResult<void>>
    handshake(const net::TlsClientParam &param, std::chrono::milliseconds timeout = net::kDefaultTlsHandshakeTimeout);
    fiber::async::Task<common::IoResult<void>>
    handshake(const net::TlsServerParam &param, std::chrono::milliseconds timeout = net::kDefaultTlsHandshakeTimeout);
    fiber::async::Task<common::IoResult<void>> shutdown(std::chrono::milliseconds timeout) override;
    fiber::async::Task<common::IoResult<void>> wait_readable(std::chrono::milliseconds timeout) override;
    common::IoErr set_read_callback(ReadyCallback callback, void *ctx) noexcept override;
    common::IoErr set_write_callback(ReadyCallback callback, void *ctx) noexcept override;
    common::IoErr set_terminal_callback(ReadyCallback callback, void *ctx) noexcept override;
    common::IoErr clear_read_callback(ReadyCallback callback, void *ctx) noexcept override;
    common::IoErr clear_write_callback(ReadyCallback callback, void *ctx) noexcept override;
    common::IoErr clear_terminal_callback(ReadyCallback callback, void *ctx) noexcept override;
    [[nodiscard]] bool read_ready() const noexcept override;
    [[nodiscard]] bool write_ready() const noexcept override;
    common::IoErr detach_for_handover() noexcept override;
    common::IoErr adopt_loop(event::EventLoop &loop) noexcept override;
    [[nodiscard]] common::IoErr ensure_state_observation() noexcept override;
    [[nodiscard]] common::IoResult<size_t> try_readv(size_t size, mem::IoBufChain &out) noexcept override;
    fiber::async::Task<common::IoResult<size_t>> readv(size_t size, mem::IoBufChain &out,
                                                       std::chrono::milliseconds timeout) override;
    [[nodiscard]] common::IoResult<size_t> try_writev(mem::IoBufChain &buf) noexcept override;
    fiber::async::Task<common::IoResult<size_t>> writev(mem::IoBufChain &buf,
                                                        std::chrono::milliseconds timeout) override;
    void close() override;
    [[nodiscard]] bool valid() const noexcept override;
    [[nodiscard]] bool terminal() const noexcept override;
    [[nodiscard]] bool peer_closed() const noexcept override;
    [[nodiscard]] int fd() const noexcept override;
    [[nodiscard]] std::string_view negotiated_alpn() const noexcept override;
    [[nodiscard]] const net::SocketAddress &remote_addr() const noexcept override;
    [[nodiscard]] event::EventLoop &loop() const noexcept override;

private:
    TlsTransport(event::EventLoop &loop, int fd, net::SocketAddress remote_addr);
    [[nodiscard]] bool handshake_done() const noexcept;

    net::TlsTcpStream stream_;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP_TRANSPORT_H
