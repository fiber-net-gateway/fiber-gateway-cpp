#ifndef FIBER_HTTP_ENDPOINT_HTTP1_ENDPOINT_H
#define FIBER_HTTP_ENDPOINT_HTTP1_ENDPOINT_H

#include <chrono>
#include <cstddef>
#include <memory>

#include "../../common/IntrusiveList.h"
#include "../Http1Connection.h"
#include "../Http1ServerOptions.h"
#include "TcpEndpointBase.h"

namespace fiber::http {

// Per-loop registry of live HTTP/1 sessions. Connections link themselves in
// from their serve coroutine frames through their intrusive hooks, so
// registration allocates nothing and the list is only ever touched on its own
// loop -- no lock, unlike the mutex-guarded shared_ptr vector this replaces.
class Http1EndpointWorker final : public TcpEndpointWorkerBase {
public:
    using TcpEndpointWorkerBase::TcpEndpointWorkerBase;

    void link(Http1Connection &connection) noexcept { connections_.push_back(connection); }
    void unlink(Http1Connection &connection) noexcept { connections_.erase(connection); }

    // Idle connections close immediately; busy ones finish the request they
    // are serving and answer with Connection: close.
    void drain() noexcept override {
        TcpEndpointWorkerBase::drain();
        for (Http1Connection *connection = connections_.front(); connection != nullptr;
             connection = connections_.next_of(*connection)) {
            connection->request_drain();
        }
    }

    // Drain budget spent: close the transports out from under whatever is
    // still running.
    void abort() noexcept override {
        for (Http1Connection *connection = connections_.front(); connection != nullptr;
             connection = connections_.next_of(*connection)) {
            connection->shutdown();
        }
    }

private:
    // Http1Connection is not standard-layout (it holds unique_ptr members), but
    // it is non-polymorphic, which is what container_of actually needs.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
    using ConnectionList = common::IntrusiveList<Http1Connection, offsetof(Http1Connection, worker_hook_)>;
#pragma GCC diagnostic pop

    ConnectionList connections_{};
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
        std::chrono::milliseconds drain_timeout{30'000};
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
