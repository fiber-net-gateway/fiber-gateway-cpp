#include <fiber/http/Http2ClientConnection.h>

#include <cerrno>
#include <memory>
#include <string_view>
#include <sys/socket.h>

#include <fiber/http/ClientHttp2Exchange.h>
#include <fiber/http/HttpTransport.h>
#include <fiber/net/TcpListener.h>
#include <fiber/net/TcpStream.h>
#include "http/TlsAlpn.h"

namespace fiber::http {

namespace {

constexpr std::string_view kH2Alpn = "h2";

} // namespace

Http2Connection::Options Http2ClientConnection::normalize_h2_options(Http2Connection::Options options) noexcept {
    options.role = Http2Connection::ConnectionRole::Client;
    return options;
}

Http2ClientConnection::Http2ClientConnection(event::EventLoop &loop, Http2Connection::Options h2) noexcept :
    loop_(&loop), conn_(normalize_h2_options(std::move(h2)), nullptr, ClientHttp2Request::factory_ops()),
    stream_gate_(conn_) {
    close_gate_.arm(conn_);
}

Http2ClientConnection::~Http2ClientConnection() {
    FIBER_ASSERT(conn_.state() == Http2Connection::State::Init || close_gate_.closed());
    FIBER_ASSERT(!close_gate_.has_joiners());
}

fiber::async::Task<common::IoResult<void>> Http2ClientConnection::connect(const net::SocketAddress &peer,
                                                                          std::chrono::milliseconds timeout,
                                                                          const net::TcpSocketOptions &tcp) noexcept {
    return connect_impl(peer, timeout, tcp, std::nullopt);
}

fiber::async::Task<common::IoResult<void>> Http2ClientConnection::connect(const net::SocketAddress &peer,
                                                                          std::chrono::milliseconds timeout,
                                                                          const HttpClientTlsOptions &tls,
                                                                          const net::TcpSocketOptions &tcp) noexcept {
    return connect_impl(peer, timeout, tcp, tls);
}

fiber::async::Task<common::IoResult<void>>
Http2ClientConnection::connect_impl(net::SocketAddress peer, std::chrono::milliseconds timeout,
                                    net::TcpSocketOptions tcp, std::optional<HttpClientTlsOptions> tls) noexcept {
    FIBER_ASSERT(loop_ != nullptr);
    if (!loop_->in_loop()) {
        co_return std::unexpected(common::IoErr::NotSupported);
    }
    if (conn_.state() != Http2Connection::State::Init) {
        co_return std::unexpected(common::IoErr::Busy);
    }

    auto connect_result = co_await net::TcpStream::connect(*loop_, peer, timeout);
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
    if (tls) {
        auto tls_param = make_http2_client_tls_param(*tls);
        auto transport_result = TlsTransport::create(*loop_, std::move(accept), tcp);
        if (!transport_result) {
            co_return std::unexpected(transport_result.error());
        }
        auto tls_transport = std::move(*transport_result);

        auto handshake_result = co_await tls_transport->handshake(tls_param, tls->handshake_timeout);
        if (!handshake_result) {
            co_return std::unexpected(handshake_result.error());
        }
        if (tls_transport->negotiated_alpn() != kH2Alpn) {
            tls_transport->close();
            co_return std::unexpected(common::IoErr::NotSupported);
        }
        transport = std::move(tls_transport);
    } else {
        auto transport_result = TcpTransport::create(*loop_, std::move(accept), tcp);
        if (!transport_result) {
            co_return std::unexpected(transport_result.error());
        }
        transport = std::move(*transport_result);
    }

    common::IoErr start_err = conn_.start(std::move(transport));
    if (start_err != common::IoErr::None) {
        co_return std::unexpected(start_err);
    }
    co_return common::IoResult<void>{};
}

fiber::async::Task<Http2Connection::CloseResult> Http2ClientConnection::wait_closed() noexcept {
    if (conn_.state() == Http2Connection::State::Init) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return co_await close_gate_.join();
}

ClientHttp2Exchange Http2ClientConnection::open_exchange(mem::BufPool &pool) noexcept {
    return ClientHttp2Exchange(*this, pool);
}

void Http2ClientConnection::shutdown(common::IoErr reason) noexcept { conn_.shutdown(reason); }

fiber::async::Task<Http2Connection::CloseResult> Http2ClientConnection::graceful_shutdown() noexcept {
    if (conn_.state() == Http2Connection::State::Init) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    conn_.graceful_shutdown();
    co_return co_await wait_closed();
}

event::EventLoop &Http2ClientConnection::loop() const noexcept {
    FIBER_ASSERT(loop_ != nullptr);
    return *loop_;
}

Http2Connection &Http2ClientConnection::http2() noexcept { return conn_; }

const Http2Connection &Http2ClientConnection::http2() const noexcept { return conn_; }

} // namespace fiber::http
