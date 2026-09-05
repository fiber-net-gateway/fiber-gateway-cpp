#ifndef FIBER_HTTP_HTTP2_CLIENT_CONNECTION_H
#define FIBER_HTTP_HTTP2_CLIENT_CONNECTION_H

#include <chrono>
#include <optional>

#include "../async/Task.h"
#include "../async/WaitGroup.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../event/EventLoop.h"
#include "../net/SocketAddress.h"
#include "../net/TcpSocketOptions.h"
#include "ClientHttp2Request.h"
#include "Http2Connection.h"
#include "Http2LocalStreamGate.h"
#include "HttpClientTlsOptions.h"

namespace fiber::http {

class ClientHttp2Exchange;

class Http2ClientConnection : public common::NonCopyable, public common::NonMovable {
public:
    // Only the HTTP/2 settings outlive connect(): they configure the owned Http2Connection here
    // and stay in force for every stream on it.
    explicit Http2ClientConnection(event::EventLoop &loop, Http2Connection::Options h2 = {}) noexcept;
    ~Http2ClientConnection();

    // Dials once; a successful connect starts HTTP/2 I/O immediately and the connection is never
    // re-dialed. `timeout` covers the TCP phase only; the TLS handshake has its own timeout in
    // HttpClientTlsOptions. Every argument is borrowed until the returned task completes,
    // including the storage behind the views in `tls`.
    fiber::async::Task<common::IoResult<void>>
    connect(const net::SocketAddress &peer, std::chrono::milliseconds timeout,
            const net::TcpSocketOptions &tcp = net::kNoDelayTcpSocketOptions) noexcept;
    fiber::async::Task<common::IoResult<void>>
    connect(const net::SocketAddress &peer, std::chrono::milliseconds timeout, const HttpClientTlsOptions &tls,
            const net::TcpSocketOptions &tcp = net::kNoDelayTcpSocketOptions) noexcept;
    fiber::async::Task<Http2Connection::CloseResult> wait_closed() noexcept;

    [[nodiscard]] ClientHttp2Exchange open_exchange(mem::BufPool &pool) noexcept;

    void shutdown(common::IoErr reason = common::IoErr::Canceled) noexcept;
    fiber::async::Task<Http2Connection::CloseResult> graceful_shutdown() noexcept;

    [[nodiscard]] event::EventLoop &loop() const noexcept;
    [[nodiscard]] Http2Connection &http2() noexcept;
    [[nodiscard]] const Http2Connection &http2() const noexcept;
    // FIFO admission for locally initiated streams on this connection.
    [[nodiscard]] Http2LocalStreamGate &stream_gate() noexcept { return stream_gate_; }
    [[nodiscard]] const std::optional<net::SocketAddress> &local_addr() const noexcept { return local_addr_; }

private:
    static Http2Connection::Options normalize_h2_options(Http2Connection::Options options) noexcept;
    static void on_http2_closed(void *ctx, Http2Connection &connection, Http2Connection::CloseResult result) noexcept;

    fiber::async::Task<common::IoResult<void>> connect_impl(net::SocketAddress peer, std::chrono::milliseconds timeout,
                                                            net::TcpSocketOptions tcp,
                                                            std::optional<HttpClientTlsOptions> tls) noexcept;

    event::EventLoop *loop_ = nullptr;
    std::optional<net::SocketAddress> local_addr_;
    fiber::async::WaitGroup close_wg_;
    Http2Connection conn_;
    Http2LocalStreamGate stream_gate_;
    common::IoErr terminal_error_ = common::IoErr::None;
    bool close_pending_ = false;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_CLIENT_CONNECTION_H
