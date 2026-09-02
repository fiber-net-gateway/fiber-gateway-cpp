#include <fiber/http/Http2ClientConnection.h>

#include <cerrno>
#include <memory>
#include <string_view>
#include <sys/socket.h>

#include <fiber/http/ClientHttp2Exchange.h>
#include <fiber/http/HttpTransport.h>
#include <fiber/net/TcpListener.h>
#include <fiber/net/TcpStream.h>

namespace fiber::http {

namespace {

constexpr std::string_view kH2Alpn = "h2";

} // namespace

net::TlsClientParam Http2ClientConnection::normalize_tls_options(net::TlsClientParam options) noexcept {
    if (options.enabled()) {
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
    loop_(&loop), peer_addr_(std::move(options.peer_addr)), tcp_options_(options.tcp),
    tls_options_(normalize_tls_options(std::move(options.tls))),
    conn_(normalize_h2_options(std::move(options.h2)), nullptr, ClientHttp2Request::factory_ops()) {}

Http2ClientConnection::~Http2ClientConnection() {
    FIBER_ASSERT(!close_pending_);
    FIBER_ASSERT(close_wg_.empty());
}

fiber::async::Task<common::IoResult<void>> Http2ClientConnection::connect(std::chrono::milliseconds timeout) noexcept {
    FIBER_ASSERT(loop_ != nullptr);
    if (!loop_->in_loop()) {
        co_return std::unexpected(common::IoErr::NotSupported);
    }
    if (conn_.state() != Http2Connection::State::Init) {
        co_return std::unexpected(common::IoErr::Busy);
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
    if (tls_options_.enabled()) {
        auto transport_result = TlsTransport::create(*loop_, std::move(accept), tls_options_, tcp_options_);
        if (!transport_result) {
            co_return std::unexpected(transport_result.error());
        }
        transport = std::move(*transport_result);

        auto handshake_result = co_await transport->handshake(tls_options_.handshake_timeout);
        if (!handshake_result) {
            co_return std::unexpected(handshake_result.error());
        }
        if (transport->negotiated_alpn() != kH2Alpn) {
            transport->close();
            co_return std::unexpected(common::IoErr::NotSupported);
        }
    } else {
        auto transport_result = TcpTransport::create(*loop_, std::move(accept), tcp_options_);
        if (!transport_result) {
            co_return std::unexpected(transport_result.error());
        }
        transport = std::move(*transport_result);
    }

    close_wg_.add();
    close_pending_ = true;
    common::IoErr start_err = conn_.start(std::move(transport), &Http2ClientConnection::on_http2_closed, this);
    if (start_err != common::IoErr::None) {
        terminal_error_ = start_err;
        close_pending_ = false;
        close_wg_.done();
        co_return std::unexpected(start_err);
    }
    co_return common::IoResult<void>{};
}

fiber::async::Task<Http2Connection::CloseResult> Http2ClientConnection::wait_closed() noexcept {
    if (conn_.state() == Http2Connection::State::Init) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_await close_wg_.join();
    if (terminal_error_ != common::IoErr::None) {
        co_return std::unexpected(terminal_error_);
    }
    co_return Http2Connection::CloseResult{};
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

void Http2ClientConnection::on_http2_closed(void *ctx, Http2Connection &,
                                            Http2Connection::CloseResult result) noexcept {
    auto *connection = static_cast<Http2ClientConnection *>(ctx);
    FIBER_ASSERT(connection != nullptr);
    FIBER_ASSERT(connection->close_pending_);
    connection->terminal_error_ = result ? common::IoErr::None : result.error();
    connection->close_pending_ = false;
    connection->close_wg_.done();
}

event::EventLoop &Http2ClientConnection::loop() const noexcept {
    FIBER_ASSERT(loop_ != nullptr);
    return *loop_;
}

Http2Connection &Http2ClientConnection::http2() noexcept { return conn_; }

const Http2Connection &Http2ClientConnection::http2() const noexcept { return conn_; }

} // namespace fiber::http
