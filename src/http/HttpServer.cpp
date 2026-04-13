#include "HttpServer.h"

#include <chrono>
#include <memory>
#include <utility>

#include "../async/Spawn.h"
#include "../common/Assert.h"
#include "../common/IoError.h"
#include "../net/TcpStream.h"
#include "Http1Connection.h"
#include "HttpTransport.h"
#include "TlsAlpn.h"

namespace fiber::http {

HttpServer::HttpServer(event::EventLoop &loop, HttpHandler handler, HttpServerOptions options,
                       event::EventLoopGroup *worker_group) :
    worker_group_(worker_group), handler_(std::move(handler)), options_(std::move(options)),
    http2_request_factory_(options_, handler_), listener_(loop) {
    FIBER_ASSERT(http2_hpack_encode_catalog_.init({}));
}

fiber::common::IoResult<void> HttpServer::bind(const net::SocketAddress &addr, const net::ListenOptions &options) {
    auto result = listener_.bind(addr, options);
    if (!result) {
        return std::unexpected(result.error());
    }
    if (options_.tls.enabled) {
        normalize_http_server_alpn(options_.tls);
        auto ctx = std::make_unique<TlsServerContext>(options_.tls);
        auto init_result = ctx->init();
        if (!init_result) {
            return std::unexpected(init_result.error());
        }
        tls_ctx_ = std::move(ctx);
    }
    return {};
}

fiber::async::DetachedTask HttpServer::serve() {
    auto *accept_loop = event::EventLoop::current_or_null();
    FIBER_ASSERT(accept_loop != nullptr);
    FIBER_ASSERT(accept_loop == &listener_.loop());

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

    std::string negotiated_alpn = transport->negotiated_alpn();
    if (negotiated_alpn.empty() || negotiated_alpn == "http/1.1") {
        co_await serve_http1(std::move(transport));
        co_return;
    }
    if (negotiated_alpn == "h2") {
        co_await serve_http2(std::move(transport));
        co_return;
    }

    transport->close();
    co_return;
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
    Http2Connection connection(make_http2_options(), &http2_request_factory_, ServerRequestFactory::ops());
    if (connection.start(std::move(transport)) != common::IoErr::None) {
        co_return;
    }
    (void) co_await connection.run();
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
    return options;
}

void HttpServer::close() { listener_.close(); }

int HttpServer::fd() const noexcept { return listener_.fd(); }

} // namespace fiber::http
