#ifndef FIBER_HTTP_ENDPOINT_HTTP1_ENDPOINT_H
#define FIBER_HTTP_ENDPOINT_HTTP1_ENDPOINT_H

#include <cstddef>
#include <memory>

#include "../Http1ServerOptions.h"
#include "Http1ConnectionRegistry.h"
#include "TcpEndpointBase.h"

namespace fiber::http {

// One HTTP/1 endpoint's presence on a single worker loop.
class Http1EndpointWorker final : public TcpEndpointWorkerBase {
public:
    using TcpEndpointWorkerBase::TcpEndpointWorkerBase;

    [[nodiscard]] Http1ConnectionRegistry &connections() noexcept { return connections_; }

    void drain() noexcept override {
        TcpEndpointWorkerBase::drain();
        connections_.drain_all();
    }

private:
    Http1ConnectionRegistry connections_{};
};

// Plaintext or TLS HTTP/1.1 endpoint. With TLS configured it offers only
// "http/1.1" over ALPN; use Http21Endpoint to negotiate HTTP/2 as well.
class Http1Endpoint final : public TcpEndpointBase {
public:
    struct Options {
        net::SocketAddress address{};
        net::ListenOptions listen{};
        net::TcpSocketOptions tcp{.no_delay = net::TcpOptionMode::Enabled};
        // Leaving this unset (no configure callback) serves plaintext HTTP/1.
        HttpServerTlsOptions tls{};
        Http1ServerOptions http1{};
        // Empty falls back to the Server's default handler.
        HttpHandler handler{};
    };

    explicit Http1Endpoint(Options options) noexcept;

    [[nodiscard]] const Http1ServerOptions &http1_options() const noexcept { return options_.http1; }

protected:
    [[nodiscard]] common::IoResult<void> on_start(Server &server) noexcept override;
    [[nodiscard]] TcpEndpointWorkerBase *make_worker(event::EventLoop &loop, std::size_t index) noexcept override;
    [[nodiscard]] net::TlsServerParam make_tls_param() const noexcept override;
    [[nodiscard]] async::Task<void> serve_connection(TcpEndpointWorkerBase &worker,
                                                     std::unique_ptr<HttpTransport> transport) noexcept override;

private:
    Options options_;
    // Resolved in on_start(); shared so a session outlives this facade if the
    // endpoint is ever torn down first.
    std::shared_ptr<const HttpHandler> handler_{};
};

} // namespace fiber::http

#endif // FIBER_HTTP_ENDPOINT_HTTP1_ENDPOINT_H
