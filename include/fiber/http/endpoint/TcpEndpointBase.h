#ifndef FIBER_HTTP_ENDPOINT_TCP_ENDPOINT_BASE_H
#define FIBER_HTTP_ENDPOINT_TCP_ENDPOINT_BASE_H

#include <cstddef>
#include <memory>
#include <vector>

#include "../../async/Task.h"
#include "../../async/WaitGroup.h"
#include "../../common/IoError.h"
#include "../../event/EventLoop.h"
#include "../../net/SocketAddress.h"
#include "../../net/TcpListener.h"
#include "../../net/TcpSocketOptions.h"
#include "../../net/TlsParams.h"
#include "../HttpServerTlsOptions.h"
#include "../HttpTransport.h"
#include "../Server.h"

namespace fiber::http {

class TcpEndpointBase;

// Shared bookkeeping for a TCP endpoint's per-loop worker: it counts the
// connection coroutines running on this loop (handshake included) and holds
// the draining flag the accept path checks.
//
// Concrete workers add their own connection registry and drain policy.
class TcpEndpointWorkerBase : public EndpointWorker {
public:
    explicit TcpEndpointWorkerBase(event::EventLoop &loop) noexcept : loop_(loop) {}

    ~TcpEndpointWorkerBase() override;
    void attach(TcpEndpointBase &endpoint, std::size_t index) noexcept;

    [[nodiscard]] event::EventLoop &loop() const noexcept { return loop_; }
    [[nodiscard]] bool draining() const noexcept { return draining_; }

    // Called on the accept loop just before the connection coroutine is
    // spawned onto this worker, and from that coroutine when it finishes. One
    // task spans handshake plus session, so a connection is accounted for from
    // the moment it is handed over.
    void begin_task() noexcept { tasks_.add(); }
    void end_task() noexcept { tasks_.done(); }

    void drain() noexcept override { draining_ = true; }

    async::Task<void> wait_stopped() noexcept override {
        co_await tasks_.join();
        co_return;
    }

private:
    TcpEndpointBase *endpoint_ = nullptr;
    std::size_t index_ = 0;
    event::EventLoop &loop_;
    async::WaitGroup tasks_{};
    bool draining_ = false;
};

// Listener, accept loop and transport setup shared by the HTTP/1, HTTP/2 and
// HTTP/1-or-2 endpoints. Accepts on the server's owner loop and hands each
// connection to a worker round-robin (design decision D2).
class TcpEndpointBase : public Endpoint {
public:
    [[nodiscard]] const net::SocketAddress &local_addr() const noexcept final { return local_addr_; }
    // Listening socket, valid between on_start() and on_stop().
    [[nodiscard]] int listener_fd() const noexcept { return listener_ ? listener_->fd() : -1; }

protected:
    TcpEndpointBase(net::SocketAddress address, net::ListenOptions listen, net::TcpSocketOptions tcp,
                    HttpServerTlsOptions tls) noexcept;
    ~TcpEndpointBase() override;

    [[nodiscard]] common::IoResult<void> on_start(Server &server) noexcept override;
    [[nodiscard]] async::Task<void> on_serve() noexcept override;
    void on_stop() noexcept override;
    [[nodiscard]] EndpointWorker *create_worker(event::EventLoop &loop, std::size_t index) noexcept final;

    // ---- protocol hooks ----

    // Allocates this endpoint's worker for `loop`; nullptr on OOM.
    [[nodiscard]] virtual TcpEndpointWorkerBase *make_worker(event::EventLoop &loop, std::size_t index) noexcept = 0;
    // ALPN set offered to TLS clients (see TlsAlpn.h helpers).
    [[nodiscard]] virtual net::TlsServerParam make_tls_param() const noexcept = 0;
    // Runs one accepted connection to completion on the worker's loop.
    [[nodiscard]] virtual async::Task<void> serve_connection(TcpEndpointWorkerBase &worker,
                                                             std::unique_ptr<HttpTransport> transport) noexcept = 0;

    [[nodiscard]] Server &server() const noexcept { return *server_; }
    [[nodiscard]] const HttpServerTlsOptions &tls() const noexcept { return tls_; }

private:
    friend class TcpEndpointWorkerBase;
    static async::DetachedTask run_accepted(TcpEndpointBase *self, TcpEndpointWorkerBase *worker,
                                            net::AcceptResult accept) noexcept;

    Server *server_ = nullptr;
    net::SocketAddress address_;
    net::ListenOptions listen_;
    net::TcpSocketOptions tcp_;
    HttpServerTlsOptions tls_;

    std::unique_ptr<net::TcpListener> listener_{};
    net::SocketAddress local_addr_{};
    // Borrowed: the Server owns the workers and outlives the accept loop.
    std::vector<TcpEndpointWorkerBase *> workers_{};
    // Owner-loop only, so no atomic.
    std::size_t next_worker_ = 0;
};

} // namespace fiber::http

#endif // FIBER_HTTP_ENDPOINT_TCP_ENDPOINT_BASE_H
