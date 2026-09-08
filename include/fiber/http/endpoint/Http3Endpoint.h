#ifndef FIBER_HTTP_ENDPOINT_HTTP3_ENDPOINT_H
#define FIBER_HTTP_ENDPOINT_HTTP3_ENDPOINT_H

#include <cstddef>
#include <memory>
#include <vector>

#include "../../async/WaitGroup.h"
#include "../../net/SocketAddress.h"
#include "../../net/TlsParams.h"
#include "../Http3ServerOptions.h"
#include "../HttpServerTlsOptions.h"
#include "../Server.h"

namespace fiber::http {

class Http3EndpointWorker;

// HTTP/3 endpoint: one QuicUdpEndpoint per worker loop, bound with SO_REUSEPORT
// when there is more than one, so the kernel spreads datagrams and no
// connection ever crosses a loop. There is no accept loop -- QUIC admission
// happens inside each shard's receive path -- so on_serve() only starts the
// shards and then parks until on_stop().
//
// Shutdown is graceful: draining stops admitting new connections and GOAWAYs
// the live ones, but keeps the UDP socket open so those connections can still
// finish. The socket closes only once the last one is gone.
class Http3Endpoint final : public Endpoint {
public:
    struct Options {
        net::SocketAddress address{};
        // When the configured port is 0 and this points at an endpoint started
        // earlier, the shards bind to that endpoint's resolved port instead.
        // That is how an HTTP/3 endpoint shares a port with the TCP endpoint
        // advertising it through Alt-Svc. Endpoints are started in insertion
        // order, so the target must have been added first.
        const Endpoint *inherit_port_from = nullptr;
        // Required: HTTP/3 has no cleartext mode.
        HttpServerTlsOptions tls{};
        Http3ServerOptions http3{};
        // Empty falls back to the Server's default handler.
        HttpHandler handler{};
    };

    explicit Http3Endpoint(Options options) noexcept;
    ~Http3Endpoint() override;

    [[nodiscard]] const net::SocketAddress &local_addr() const noexcept override { return local_addr_; }
    [[nodiscard]] std::size_t shard_count() const noexcept { return workers_.size(); }

protected:
    [[nodiscard]] common::IoResult<void> on_start(Server &server) noexcept override;
    [[nodiscard]] async::Task<void> on_serve() noexcept override;
    void on_stop() noexcept override;
    [[nodiscard]] EndpointWorker *create_worker(event::EventLoop &loop, std::size_t index) noexcept override;

private:
    friend class Http3EndpointWorker;
    Options options_;
    // Stable storage: every shard's ServerAdmissionOptions borrows this for the
    // lifetime of the endpoint, built once from options_.tls at start.
    net::TlsServerParam tls_param_{};
    std::shared_ptr<const HttpHandler> handler_{};
    net::SocketAddress local_addr_{};

    // Built in on_start() and handed to the Server one at a time by
    // create_worker(); anything left here was never claimed.
    std::vector<std::unique_ptr<Http3EndpointWorker>> pending_workers_{};
    // Borrowed: the Server owns the workers once they are claimed.
    std::vector<Http3EndpointWorker *> workers_{};
    // Released by on_stop(), which is what ends on_serve().
    async::WaitGroup serve_gate_{};
    bool gate_opened_ = false;
};

} // namespace fiber::http

#endif // FIBER_HTTP_ENDPOINT_HTTP3_ENDPOINT_H
