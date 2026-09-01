#ifndef FIBER_HTTP_HTTP2_CLIENT_CONNECTION_H
#define FIBER_HTTP_HTTP2_CLIENT_CONNECTION_H

#include <chrono>
#include <memory>
#include <optional>

#include "../async/Task.h"
#include "../async/WaitGroup.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../event/EventLoop.h"
#include "../net/SocketAddress.h"
#include "../net/TcpSocketOptions.h"
#include "../net/TlsContext.h"
#include "ClientHttp2Request.h"
#include "Http2Connection.h"

namespace fiber::http {

class ClientHttp2Exchange;

class Http2ClientConnection : public common::NonCopyable, public common::NonMovable {
public:
    struct Options {
        net::SocketAddress peer_addr{};
        net::TcpSocketOptions tcp{.no_delay = net::TcpOptionMode::Enabled};
        net::TlsClientConnectionOptions tls{};
        Http2Connection::Options h2{};
    };

    Http2ClientConnection(event::EventLoop &loop, Options options) noexcept;
    ~Http2ClientConnection();

    // timeout applies to the TCP connect phase. TLS handshake timeout is configured separately.
    // A successful connect starts HTTP/2 I/O immediately.
    fiber::async::Task<common::IoResult<void>> connect(std::chrono::milliseconds timeout) noexcept;
    fiber::async::Task<Http2Connection::CloseResult> wait_closed() noexcept;

    [[nodiscard]] ClientHttp2Exchange open_exchange(mem::BufPool &pool) noexcept;

    void shutdown(common::IoErr reason = common::IoErr::Canceled) noexcept;
    fiber::async::Task<Http2Connection::CloseResult> graceful_shutdown() noexcept;

    [[nodiscard]] event::EventLoop &loop() const noexcept;
    [[nodiscard]] Http2Connection &http2() noexcept;
    [[nodiscard]] const Http2Connection &http2() const noexcept;
    [[nodiscard]] const std::optional<net::SocketAddress> &local_addr() const noexcept { return local_addr_; }

private:
    static net::TlsClientConnectionOptions normalize_tls_options(net::TlsClientConnectionOptions options) noexcept;
    static Http2Connection::Options normalize_h2_options(Http2Connection::Options options) noexcept;
    static void on_http2_closed(void *ctx, Http2Connection &connection, Http2Connection::CloseResult result) noexcept;

    event::EventLoop *loop_ = nullptr;
    net::SocketAddress peer_addr_{};
    net::TcpSocketOptions tcp_options_{};
    std::optional<net::SocketAddress> local_addr_;
    net::TlsClientConnectionOptions tls_options_{};
    fiber::async::WaitGroup close_wg_;
    Http2Connection conn_;
    common::IoErr terminal_error_ = common::IoErr::None;
    bool close_pending_ = false;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_CLIENT_CONNECTION_H
