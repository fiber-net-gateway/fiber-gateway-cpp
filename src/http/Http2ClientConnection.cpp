#include "Http2ClientConnection.h"

#include <cerrno>
#include <memory>
#include <string_view>
#include <sys/socket.h>

#include "../net/TcpListener.h"
#include "../net/TcpStream.h"
#include "ClientHttp2Exchange.h"
#include "HttpTransport.h"

namespace fiber::http {

namespace {

constexpr std::string_view kH2Alpn = "h2";

} // namespace

net::TlsOptions Http2ClientConnection::normalize_tls_options(net::TlsOptions options) noexcept {
    if (options.enabled) {
        options.alpn.clear();
        options.alpn.push_back(std::string(kH2Alpn));
    }
    return options;
}

Http2Connection::Options Http2ClientConnection::normalize_h2_options(Http2Connection::Options options) noexcept {
    options.role = Http2Connection::ConnectionRole::Client;
    return options;
}

Http2ClientConnection::Http2ClientConnection(event::EventLoop &loop, Options options) noexcept :
    loop_(&loop), peer_addr_(std::move(options.peer_addr)),
    tls_ctx_(normalize_tls_options(std::move(options.tls)), false, false),
    conn_(normalize_h2_options(std::move(options.h2)), nullptr, ClientHttp2Request::factory_ops()) {}

fiber::async::Task<common::IoResult<void>> Http2ClientConnection::connect(std::chrono::milliseconds timeout,
                                                                          Http2Connection::ClosedCallback on_closed,
                                                                          void *closed_ctx) noexcept {
    FIBER_ASSERT(loop_ != nullptr);
    if (!loop_->in_loop()) {
        co_return std::unexpected(common::IoErr::NotSupported);
    }
    if (conn_.state() != Http2Connection::State::Init) {
        co_return std::unexpected(common::IoErr::Busy);
    }

    if (tls_ctx_.options().enabled) {
        auto init_result = tls_ctx_.init();
        if (!init_result) {
            co_return std::unexpected(init_result.error());
        }
    }

    auto connect_result = co_await net::TcpStream::connect(*loop_, peer_addr_, timeout);
    if (!connect_result) {
        co_return std::unexpected(connect_result.error());
    }

    sockaddr_storage local_storage{};
    socklen_t local_len = sizeof(local_storage);
    if (::getsockname(connect_result->fd(), reinterpret_cast<sockaddr *>(&local_storage), &local_len) != 0) {
        co_return std::unexpected(common::io_err_from_errno(errno));
    }
    net::SocketAddress local;
    if (!net::SocketAddress::from_sockaddr(reinterpret_cast<const sockaddr *>(&local_storage), local_len, local)) {
        co_return std::unexpected(common::IoErr::NotSupported);
    }
    local_addr_ = local;

    net::AcceptResult accept(connect_result->release_fd(), connect_result->take_peer());
    std::unique_ptr<HttpTransport> transport;
    if (tls_ctx_.options().enabled) {
        auto transport_result = TlsTransport::create(*loop_, std::move(accept), tls_ctx_);
        if (!transport_result) {
            co_return std::unexpected(transport_result.error());
        }
        transport = std::move(*transport_result);

        auto handshake_result = co_await transport->handshake(tls_ctx_.options().handshake_timeout);
        if (!handshake_result) {
            co_return std::unexpected(handshake_result.error());
        }
        if (transport->negotiated_alpn() != kH2Alpn) {
            transport->close();
            co_return std::unexpected(common::IoErr::NotSupported);
        }
    } else {
        auto transport_result = TcpTransport::create(*loop_, std::move(accept));
        if (!transport_result) {
            co_return std::unexpected(transport_result.error());
        }
        transport = std::move(*transport_result);
    }

    common::IoErr start_err = conn_.start(std::move(transport), on_closed, closed_ctx);
    if (start_err != common::IoErr::None) {
        co_return std::unexpected(start_err);
    }
    co_return common::IoResult<void>{};
}

fiber::async::Task<Http2Connection::RunResult> Http2ClientConnection::run() noexcept { co_return co_await conn_.run(); }

ClientHttp2Exchange Http2ClientConnection::open_exchange(mem::BufPool &pool) noexcept {
    return ClientHttp2Exchange(*this, pool);
}

void Http2ClientConnection::shutdown(common::IoErr reason) noexcept { conn_.shutdown(reason); }

void Http2ClientConnection::graceful_shutdown() noexcept { conn_.graceful_shutdown(); }

event::EventLoop &Http2ClientConnection::loop() const noexcept {
    FIBER_ASSERT(loop_ != nullptr);
    return *loop_;
}

Http2Connection &Http2ClientConnection::http2() noexcept { return conn_; }

const Http2Connection &Http2ClientConnection::http2() const noexcept { return conn_; }

} // namespace fiber::http
