#include <fiber/http/HttpClientDialer.h>

#include <cerrno>
#include <sys/socket.h>
#include <utility>

#include <fiber/common/Assert.h>
#include <fiber/http/HttpTransport.h>
#include <fiber/net/TcpConnector.h>
#include <fiber/net/TcpListener.h>
#include <fiber/net/TcpStream.h>
#include "http/TlsAlpn.h"

namespace fiber::http {

std::optional<HttpProtocol> http_protocol_from_alpn(std::string_view alpn) noexcept {
    if (alpn == "h2") {
        return HttpProtocol::Http2;
    }
    if (alpn == "http/1.1" || alpn == "http/1.0") {
        return HttpProtocol::Http1;
    }
    if (alpn == "h3") {
        return HttpProtocol::Http3;
    }
    return std::nullopt;
}

fiber::async::Task<common::IoResult<HttpClientDialResult>> http_client_dial(event::EventLoop &loop,
                                                                            HttpClientDialRequest request) noexcept {
    if (!loop.in_loop()) {
        co_return std::unexpected(common::IoErr::NotSupported);
    }

    std::optional<net::TcpStream::ConnectInfant> infant;
    if (request.peer) {
        auto connect_result = co_await net::TcpStream::connect(loop, *request.peer, request.happy.total_timeout);
        if (!connect_result) {
            co_return std::unexpected(connect_result.error());
        }
        infant.emplace(std::move(*connect_result));
    } else {
        auto connect_result = co_await net::TcpConnector::connect(loop, request.addresses, request.happy);
        if (!connect_result) {
            co_return std::unexpected(connect_result.error().code);
        }
        infant.emplace(std::move(*connect_result));
    }
    FIBER_ASSERT(infant.has_value());

    HttpClientDialResult result;
    result.protocol = request.default_protocol;
    result.peer = infant->peer();

    if (request.need_local_addr) {
        sockaddr_storage local_storage{};
        socklen_t local_len = sizeof(local_storage);
        if (::getsockname(infant->fd(), reinterpret_cast<sockaddr *>(&local_storage), &local_len) != 0) {
            co_return std::unexpected(common::io_err_from_errno(errno));
        }
        net::SocketAddress local;
        if (!net::SocketAddress::from_sockaddr(reinterpret_cast<const sockaddr *>(&local_storage), local_len, local)) {
            co_return std::unexpected(common::IoErr::NotSupported);
        }
        result.local = local;
    }

    net::AcceptResult accept(infant->release_fd(), infant->take_peer());
    if (request.tls != nullptr) {
        auto tls_param = make_negotiating_client_tls_param(*request.tls, request.alpn);
        auto transport_result = TlsTransport::create(loop, std::move(accept), request.tcp);
        if (!transport_result) {
            co_return std::unexpected(transport_result.error());
        }
        auto tls_transport = std::move(*transport_result);

        auto handshake_result = co_await tls_transport->handshake(tls_param, request.tls->handshake_timeout);
        if (!handshake_result) {
            tls_transport->close();
            co_return std::unexpected(handshake_result.error());
        }

        const std::string_view negotiated = tls_transport->negotiated_alpn();
        if (!negotiated.empty()) {
            auto protocol = http_protocol_from_alpn(negotiated);
            // A peer that answers with something outside the offered set is unusable here: there
            // is no implementation to hand the transport to.
            if (!protocol) {
                tls_transport->close();
                co_return std::unexpected(common::IoErr::NotSupported);
            }
            result.protocol = *protocol;
        }
        result.transport = std::move(tls_transport);
    } else {
        auto transport_result = TcpTransport::create(loop, std::move(accept), request.tcp);
        if (!transport_result) {
            co_return std::unexpected(transport_result.error());
        }
        result.transport = std::move(*transport_result);
    }

    if (!result.transport || !result.transport->valid()) {
        if (result.transport) {
            result.transport->close();
        }
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return result;
}

} // namespace fiber::http
