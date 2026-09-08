#include <fiber/http/endpoint/Http1Endpoint.h>

#include <new>
#include <utility>

#include <fiber/common/Assert.h>
#include "http/TlsAlpn.h"

namespace fiber::http {

Http1Endpoint::Http1Endpoint(Options options) noexcept :
    TcpEndpointBase(options.address, options.listen, options.tcp, options.tls, options.drain_timeout),
    options_(std::move(options)) {}

common::IoResult<void> Http1Endpoint::on_start(Server &server) noexcept {
    auto started = TcpEndpointBase::on_start(server);
    if (!started) {
        return started;
    }

    const HttpHandler &handler = options_.handler ? options_.handler : server.default_handler();
    handler_ = std::make_shared<const HttpHandler>(handler);
    if (!handler_) {
        return std::unexpected(common::IoErr::NoMem);
    }
    return {};
}

TcpEndpointWorkerBase *Http1Endpoint::make_worker(event::EventLoop &loop, std::size_t index) noexcept {
    (void) index;
    return new (std::nothrow) Http1EndpointWorker(loop);
}

net::TlsServerParam Http1Endpoint::make_tls_param() const noexcept { return make_http1_server_tls_param(options_.tls); }

async::Task<void> Http1Endpoint::serve_connection(TcpEndpointWorkerBase &base_worker,
                                                  std::unique_ptr<HttpTransport> transport) noexcept {
    auto &worker = static_cast<Http1EndpointWorker &>(base_worker);

    // The session lives on this coroutine's frame: no per-connection heap
    // allocation, and the worker reaches it through its intrusive hook.
    Http1Connection connection(std::move(transport), *handler_, options_.http1, handler_);
    worker.link(connection);
    // drain() may have walked the list before this connection joined it.
    if (worker.draining()) {
        connection.request_drain();
    }

    co_await connection.run();

    worker.unlink(connection);
    co_return;
}

} // namespace fiber::http
