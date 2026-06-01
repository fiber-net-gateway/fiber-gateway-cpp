#ifndef FIBER_HTTP_HTTP2_CLIENT_CONNECTION_H
#define FIBER_HTTP_HTTP2_CLIENT_CONNECTION_H

#include <memory>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../event/EventLoop.h"
#include "../net/SocketAddress.h"
#include "../net/TlsContext.h"
#include "ClientHttp2Request.h"
#include "Http2Connection.h"

namespace fiber::http {

class ClientHttp2Exchange;

class Http2ClientConnection : public common::NonCopyable, public common::NonMovable {
public:
    struct Options {
        net::SocketAddress peer_addr{};
        net::TlsOptions tls{};
        Http2Connection::Options h2{};
    };

    Http2ClientConnection(event::EventLoop &loop, Options options) noexcept;

    fiber::async::Task<common::IoResult<void>> connect() noexcept;
    fiber::async::Task<Http2Connection::RunResult> run() noexcept;

    [[nodiscard]] ClientHttp2Exchange open_exchange(mem::BufPool &pool) noexcept;

    void shutdown(common::IoErr reason = common::IoErr::Canceled) noexcept;
    void graceful_shutdown() noexcept;

    [[nodiscard]] event::EventLoop &loop() const noexcept;
    [[nodiscard]] Http2Connection &http2() noexcept;
    [[nodiscard]] const Http2Connection &http2() const noexcept;

private:
    static net::TlsOptions normalize_tls_options(net::TlsOptions options) noexcept;
    static Http2Connection::Options normalize_h2_options(Http2Connection::Options options) noexcept;

    event::EventLoop *loop_ = nullptr;
    net::SocketAddress peer_addr_{};
    net::TlsContext tls_ctx_;
    Http2Connection conn_;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_CLIENT_CONNECTION_H
