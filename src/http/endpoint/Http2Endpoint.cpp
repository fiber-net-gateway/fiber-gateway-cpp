#include <fiber/http/endpoint/Http2Endpoint.h>

#include <new>
#include <utility>

#include <fiber/common/Assert.h>
#include <fiber/http/Http1Connection.h>
#include "http/TlsAlpn.h"

namespace fiber::http {

Http2Endpoint::Http2Endpoint(Options options) noexcept :
    TcpEndpointBase(options.address, options.listen, options.tcp, options.tls), options_(std::move(options)) {}

common::IoResult<void> Http2Endpoint::on_start(Server &server) noexcept {
    if (options_.http2.idle_timeout < std::chrono::milliseconds::zero()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    auto started = TcpEndpointBase::on_start(server);
    if (!started) {
        return started;
    }

    const HttpHandler &handler = options_.handler ? options_.handler : server.default_handler();
    handler_ = std::make_shared<const HttpHandler>(handler);
    if (!handler_) {
        TcpEndpointBase::on_stop();
        return std::unexpected(common::IoErr::NoMem);
    }
    request_factory_ = std::unique_ptr<ServerRequestFactory>(new (std::nothrow) ServerRequestFactory(handler_));
    if (!request_factory_) {
        TcpEndpointBase::on_stop();
        return std::unexpected(common::IoErr::NoMem);
    }
    return {};
}

TcpEndpointWorkerBase *Http2Endpoint::make_worker(event::EventLoop &loop, std::size_t index) noexcept {
    (void) index;
    return new (std::nothrow) Http2EndpointWorker(loop);
}

net::TlsServerParam Http2Endpoint::make_tls_param() const noexcept {
    return options_.allow_http1 ? make_http_server_tls_param(options_.tls) : make_http2_server_tls_param(options_.tls);
}

Http2Endpoint::Protocol Http2Endpoint::select_protocol(std::string_view alpn) const noexcept {
    if (!tls().enabled()) {
        // No ALPN to negotiate with, and Upgrade: h2c is not supported (D6).
        return options_.allow_http1 ? Protocol::Http1 : Protocol::Http2;
    }
    if (alpn == "h2") {
        return Protocol::Http2;
    }
    if (alpn.empty() || alpn == "http/1.1") {
        return options_.allow_http1 ? Protocol::Http1 : Protocol::Unsupported;
    }
    return Protocol::Unsupported;
}

Http2Connection::Options Http2Endpoint::make_connection_options() const noexcept {
    Http2Connection::Options options;
    options.role = Http2Connection::ConnectionRole::Server;
    options.read_timeout = options_.http2.read_timeout;
    options.write_timeout = options_.http2.write_timeout;
    options.enable_connect_protocol = options_.http2.enable_connect_protocol;
    return options;
}

async::Task<void> Http2Endpoint::serve_connection(TcpEndpointWorkerBase &base_worker,
                                                  std::unique_ptr<HttpTransport> transport) noexcept {
    auto &worker = static_cast<Http2EndpointWorker &>(base_worker);
    switch (select_protocol(transport->negotiated_alpn())) {
        case Protocol::Http1:
            co_await serve_http1(worker, std::move(transport));
            co_return;
        case Protocol::Http2:
            co_await serve_http2(worker, std::move(transport));
            co_return;
        case Protocol::Unsupported:
            transport->close();
            co_return;
    }
    co_return;
}

async::Task<void> Http2Endpoint::serve_http1(Http2EndpointWorker &worker,
                                             std::unique_ptr<HttpTransport> transport) noexcept {
    // Same shape as Http1Endpoint: the session lives on this coroutine's frame
    // and the worker reaches it through its intrusive hook.
    Http1Connection connection(std::move(transport), *handler_, options_.http1, handler_);
    worker.http1_connections().link(connection);
    if (worker.draining()) {
        connection.request_drain();
    }

    co_await connection.run();

    worker.http1_connections().unlink(connection);
    co_return;
}

async::Task<void> Http2Endpoint::serve_http2(Http2EndpointWorker &worker,
                                             std::unique_ptr<HttpTransport> transport) noexcept {
    Http2ServerConnection connection(worker.loop(), make_connection_options(), *request_factory_,
                                     options_.http2.idle_timeout);
    if (connection.start(std::move(transport)) != common::IoErr::None) {
        // A connection that reached Closed still has to be awaited so its
        // teardown completes before the frame goes away.
        if (connection.http2().state() == Http2Connection::State::Closed) {
            (void) co_await connection.wait_closed();
        }
        co_return;
    }

    worker.http2_connections().link(connection);
    // drain() may have walked the registry before this session joined it.
    if (worker.draining()) {
        connection.request_drain();
    }

    (void) co_await connection.wait_closed();

    worker.http2_connections().unlink(connection);
    co_return;
}

} // namespace fiber::http
