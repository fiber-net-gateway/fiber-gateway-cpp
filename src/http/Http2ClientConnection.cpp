#include <fiber/http/Http2ClientConnection.h>

#include <memory>
#include <string_view>
#include <utility>

#include <fiber/http/ClientHttp2Exchange.h>
#include <fiber/http/HttpClientDialer.h>
#include <fiber/http/HttpTransport.h>

namespace fiber::http {

namespace {

constexpr std::string_view kH2Alpn = "h2";
constexpr std::string_view kHttp2AlpnList[] = {kH2Alpn};

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

    HttpClientDialRequest request;
    request.peer = peer;
    request.happy.total_timeout = timeout;
    request.tcp = tcp;
    request.default_protocol = HttpProtocol::Http2;
    request.need_local_addr = true;
    if (tls) {
        request.tls = &*tls;
        request.alpn = kHttp2AlpnList;
    }
    auto dial_result = co_await http_client_dial(*loop_, std::move(request));
    if (!dial_result) {
        co_return std::unexpected(dial_result.error());
    }

    common::IoErr adopt_error = adopt(std::move(dial_result->transport), std::move(dial_result->local));
    if (adopt_error != common::IoErr::None) {
        co_return std::unexpected(adopt_error);
    }
    co_return common::IoResult<void>{};
}

common::IoErr Http2ClientConnection::adopt(std::unique_ptr<HttpTransport> transport,
                                           std::optional<net::SocketAddress> local) noexcept {
    auto reject = [&](common::IoErr error) noexcept {
        if (transport) {
            transport->close();
        }
        return error;
    };

    FIBER_ASSERT(loop_ != nullptr);
    if (!loop_->in_loop()) {
        return reject(common::IoErr::NotSupported);
    }
    if (conn_.state() != Http2Connection::State::Init) {
        return reject(common::IoErr::Busy);
    }
    if (!transport || !transport->valid()) {
        return reject(common::IoErr::Invalid);
    }
    if (&transport->loop() != loop_) {
        return reject(common::IoErr::Invalid);
    }
    // An empty ALPN is a cleartext prior-knowledge connection; anything else must be "h2".
    const std::string_view alpn = transport->negotiated_alpn();
    if (!alpn.empty() && alpn != kH2Alpn) {
        return reject(common::IoErr::NotSupported);
    }

    local_addr_ = std::move(local);
    // start() owns the transport from here on, including closing it on a failed handshake flight.
    return conn_.start(std::move(transport));
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
