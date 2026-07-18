#include "HttpServer.h"

#include <cerrno>
#include <chrono>
#include <memory>
#include <new>
#include <string_view>
#include <sys/socket.h>
#include <utility>

#include "../async/Spawn.h"
#include "../common/Assert.h"
#include "../common/IoError.h"
#include "../net/TcpStream.h"
#include "Http1Connection.h"
#include "HttpTransport.h"
#include "TlsAlpn.h"

namespace fiber::http {

namespace {

enum class SelectedProtocol {
    Http1,
    Http2,
    Unsupported,
};

SelectedProtocol select_protocol(std::string_view alpn) noexcept {
    if (alpn.empty() || alpn == "http/1.1") {
        return SelectedProtocol::Http1;
    }
    if (alpn == "h2") {
        return SelectedProtocol::Http2;
    }
    return SelectedProtocol::Unsupported;
}

common::IoResult<net::SocketAddress> resolve_local_addr(int fd) noexcept {
    sockaddr_storage storage{};
    socklen_t len = sizeof(storage);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&storage), &len) != 0) {
        return std::unexpected(common::io_err_from_errno(errno));
    }

    net::SocketAddress local;
    if (!net::SocketAddress::from_sockaddr(reinterpret_cast<const sockaddr *>(&storage), len, local)) {
        return std::unexpected(common::IoErr::NotSupported);
    }
    return local;
}

} // namespace

HttpServer::HttpServer(event::EventLoop &loop, HttpHandler handler, HttpServerOptions options,
                       event::EventLoopGroup *worker_group) :
    worker_group_(worker_group), handler_(std::move(handler)), options_(std::move(options)),
    http2_request_factory_(options_, handler_), listener_(loop) {
    FIBER_ASSERT(http2_hpack_encode_catalog_.init({}));
}

fiber::common::IoResult<void> HttpServer::bind(const net::SocketAddress &addr, const net::ListenOptions &options) {
    if (options_.http3.enabled && !options_.tls.enabled) {
        return std::unexpected(common::IoErr::Invalid);
    }

    auto result = listener_.bind(addr, options);
    if (!result) {
        return std::unexpected(result.error());
    }

    net::SocketAddress bound_addr = addr;
    auto local_addr = resolve_local_addr(listener_.fd());
    if (!local_addr) {
        listener_.close();
        return std::unexpected(local_addr.error());
    }
    bound_addr = *local_addr;

    if (options_.tls.enabled) {
        normalize_http_server_alpn(options_.tls);
        auto ctx = std::make_unique<net::TlsServerContext>(options_.tls);
        auto init_result = ctx->init();
        if (!init_result) {
            return std::unexpected(init_result.error());
        }
        tls_ctx_ = std::move(ctx);
    }
    if (options_.http3.enabled) {
        http3_server_ = std::make_unique<Http3Server>(listener_.loop(), handler_, options_, worker_group_);
        if (!http3_server_) {
            listener_.close();
            return std::unexpected(common::IoErr::NoMem);
        }
        auto http3_bound = http3_server_->bind(bound_addr);
        if (!http3_bound) {
            http3_server_.reset();
            listener_.close();
            return std::unexpected(http3_bound.error());
        }
    }
    return {};
}

fiber::async::DetachedTask HttpServer::serve() {
    auto *accept_loop = event::EventLoop::current_or_null();
    FIBER_ASSERT(accept_loop != nullptr);
    FIBER_ASSERT(accept_loop == &listener_.loop());

    if (http3_server_) {
        http3_server_->serve();
    }

    while (listener_.valid()) {
        auto accept_result = co_await listener_.accept();
        if (!accept_result) {
            if (accept_result.error() == common::IoErr::Canceled || accept_result.error() == common::IoErr::BadFd) {
                break;
            }
            continue;
        }

        auto accept = std::move(*accept_result);
        fiber::async::spawn(select_connection_loop(),
                            [this, accept = std::move(accept)]() mutable -> fiber::async::DetachedTask {
                                return handle_connection(std::move(accept));
                            });
    }
    co_return;
}

event::EventLoop &HttpServer::select_connection_loop() noexcept {
    if (!worker_group_ || worker_group_->size() == 0) {
        return event::EventLoop::current();
    }
    std::size_t index = next_loop_index_.fetch_add(1, std::memory_order_relaxed);
    return worker_group_->at(index % worker_group_->size());
}

fiber::async::DetachedTask HttpServer::handle_connection(net::AcceptResult accept) {
    std::unique_ptr<HttpTransport> transport;
    if (options_.tls.enabled) {
        if (!tls_ctx_) {
            co_return;
        }
        auto tls_result = TlsTransport::create(event::EventLoop::current(), std::move(accept), *tls_ctx_);
        if (!tls_result) {
            co_return;
        }
        transport = std::move(*tls_result);
        auto hs_result = co_await transport->handshake(options_.tls.handshake_timeout);
        if (!hs_result) {
            transport->close();
            co_return;
        }
    } else {
        auto tcp_result = TcpTransport::create(event::EventLoop::current(), std::move(accept));
        if (!tcp_result) {
            co_return;
        }
        transport = std::move(*tcp_result);
    }

    if (!transport) {
        co_return;
    }

    if (!options_.tls.enabled) {
        co_await serve_http1(std::move(transport));
        co_return;
    }

    switch (select_protocol(transport->negotiated_alpn())) {
        case SelectedProtocol::Http1:
            co_await serve_http1(std::move(transport));
            co_return;
        case SelectedProtocol::Http2:
            co_await serve_http2(std::move(transport));
            co_return;
        case SelectedProtocol::Unsupported:
            transport->close();
            co_return;
    }
}

fiber::async::Task<void> HttpServer::serve_http1(std::unique_ptr<HttpTransport> transport) {
    if (!transport) {
        co_return;
    }
    Http1Connection connection(nullptr, std::move(transport), handler_, options_);
    co_await connection.run();
    co_return;
}

fiber::async::Task<void> HttpServer::serve_http2(std::unique_ptr<HttpTransport> transport) {
    if (!transport) {
        co_return;
    }
    auto *connection = new (std::nothrow)
            Http2Connection(make_http2_options(), &http2_request_factory_, ServerRequestFactory::ops());
    if (!connection) {
        co_return;
    }
    auto on_closed = [](void *, Http2Connection &closed_connection, Http2Connection::RunResult) noexcept {
        delete &closed_connection;
    };
    if (connection->start(std::move(transport), on_closed) != common::IoErr::None) {
        delete connection;
    }
    co_return;
}

Http2Connection::Options HttpServer::make_http2_options() const noexcept {
    Http2Connection::Options options;
    options.role = Http2Connection::ConnectionRole::Server;
    options.outbound_hpack_catalog = &http2_hpack_encode_catalog_;
    options.read_timeout = std::chrono::duration_cast<std::chrono::milliseconds>(options_.keep_alive_timeout);
    options.write_timeout = std::chrono::duration_cast<std::chrono::milliseconds>(options_.write_timeout);
    options.keepalive_ping_interval =
            std::chrono::duration_cast<std::chrono::milliseconds>(options_.keep_alive_timeout);
    options.enable_connect_protocol = options_.enable_extended_connect;
    return options;
}

void HttpServer::close() {
    if (http3_server_) {
        http3_server_->close();
    }
    listener_.close();
}

int HttpServer::fd() const noexcept { return listener_.fd(); }

} // namespace fiber::http
