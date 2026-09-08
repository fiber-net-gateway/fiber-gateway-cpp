#include <fiber/http/endpoint/Http3Endpoint.h>

#include <algorithm>
#include <new>
#include <utility>

#include <fiber/async/Spawn.h>
#include <fiber/common/Assert.h>
#include <fiber/quic/QuicUdpEndpoint.h>
#include "http/Http3ServerConnection.h"
#include "http/TlsAlpn.h"

namespace fiber::http {

// One shard: the UDP endpoint bound to this worker's loop, plus the sessions
// admitted through it. Everything here runs on that loop, except construction
// and init() (startup thread, before the loop ever sees the socket).
class Http3EndpointWorker final : public EndpointWorker {
public:
    Http3EndpointWorker(event::EventLoop &loop, std::shared_ptr<const HttpHandler> handler,
                        const Http3ServerOptions &options) noexcept :
        loop_(loop), handler_(std::move(handler)), options_(&options) {}

    ~Http3EndpointWorker() override {
        // Either wait_stopped() already closed it on this loop, or the endpoint
        // never started and closing here is safe (see QuicUdpEndpoint::close).
        endpoint_.close();
        if (owner_) {
            owner_->workers_[index_] = nullptr;
        }
    }

    void attach(Http3Endpoint &owner, std::size_t index) noexcept {
        owner_ = &owner;
        index_ = index;
    }

    [[nodiscard]] event::EventLoop &loop() const noexcept { return loop_; }

    [[nodiscard]] common::IoResult<void> init(const net::SocketAddress &addr, bool reuse_port,
                                              const net::TlsServerParam &tls) noexcept {
        quic::QuicUdpEndpoint::EndpointOptions endpoint_options{};
        endpoint_options.bind_addr = addr;
        endpoint_options.max_connections = options_->max_connections_per_shard;
        endpoint_options.retained_storage_limit = options_->retained_storage_limit;
        endpoint_options.udp = options_->udp;
        endpoint_options.udp.reuse_addr = true;
        endpoint_options.udp.reuse_port = endpoint_options.udp.reuse_port || reuse_port;
        endpoint_options.send = options_->send;

        quic::QuicUdpEndpoint::ServerAdmissionOptions admission{};
        admission.tls = &tls;
        admission.transport = options_->transport;
        admission.transport.max_ack_delay = options_->max_ack_delay;
        admission.transport.ack_delay_exponent = options_->ack_delay_exponent;
        admission.keepalive_interval = options_->keepalive_interval;
        admission.recv_flow = options_->recv_flow;
        admission.retry = options_->retry;
        admission.issue_new_token = options_->issue_new_token;
        admission.enable_early_data = options_->enable_early_data;
        admission.connection_owner = this;
        admission.create_connection = &Http3EndpointWorker::create_connection_op;

        return endpoint_.init(loop_, endpoint_options, admission);
    }

    [[nodiscard]] const net::SocketAddress &local_addr() const noexcept { return endpoint_.local_addr(); }

    // Starts this shard's receive path, on its own loop.
    void post_start() noexcept {
        if (loop_.in_loop()) {
            start();
            return;
        }
        loop_.post<Http3EndpointWorker, &Http3EndpointWorker::start_entry_, &Http3EndpointWorker::on_start>(*this);
    }

    void start() noexcept {
        FIBER_ASSERT(loop_.in_loop());
        if (!endpoint_.valid()) {
            return;
        }
        auto started = endpoint_.start();
        if (!started && started.error() != common::IoErr::Already) {
            endpoint_.close();
        }
    }

    // Stop admitting, and GOAWAY what is already running. The UDP socket stays
    // open: the live sessions still need to send and receive on it.
    void drain() noexcept override {
        admitting_ = false;
        connections_.drain_all();
    }

    async::Task<void> wait_stopped() noexcept override {
        co_await live_.join();
        // Last session is gone, so the socket has no more work.
        endpoint_.close();
        co_return;
    }

private:
    static void on_start(Http3EndpointWorker *self) noexcept { self->start(); }

    [[nodiscard]] static quic::QuicConnection::Lease
    create_connection_op(void *owner, const quic::QuicConnection::Options &options) noexcept {
        return static_cast<Http3EndpointWorker *>(owner)->create_connection(options);
    }

    [[nodiscard]] quic::QuicConnection::Lease create_connection(const quic::QuicConnection::Options &options) noexcept {
        if (!admitting_) {
            return {};
        }
        live_.add();
        Http3ServerConnection *connection =
                Http3ServerConnection::create(options, handler_, *options_, this, connection_ops());
        if (connection == nullptr) {
            live_.done();
            return {};
        }
        connections_.link(*connection);
        connection->start();
        return quic::QuicConnection::Lease::adopt(&connection->quic());
    }

    static const Http3ServerConnection::Ops &connection_ops() noexcept {
        static const Http3ServerConnection::Ops ops{&Http3EndpointWorker::on_connection_closed};
        return ops;
    }

    // On the shard's loop, just before the session is destroyed.
    static void on_connection_closed(void *owner, Http3ServerConnection &connection) noexcept {
        auto *self = static_cast<Http3EndpointWorker *>(owner);
        self->connections_.unlink(connection);
        self->live_.done();
    }

    Http3Endpoint *owner_ = nullptr;
    std::size_t index_ = 0;
    event::EventLoop &loop_;
    std::shared_ptr<const HttpHandler> handler_;
    const Http3ServerOptions *options_;
    event::EventLoop::NotifyEntry start_entry_{};
    quic::QuicUdpEndpoint endpoint_{};
    Http3ConnectionRegistry connections_{};
    async::WaitGroup live_{};
    bool admitting_ = true;
};

Http3Endpoint::Http3Endpoint(Options options) noexcept : options_(std::move(options)) { serve_gate_.add(); }

Http3Endpoint::~Http3Endpoint() {
    FIBER_ASSERT(std::all_of(workers_.begin(), workers_.end(), [](auto *worker) { return worker == nullptr; }));
    if (!gate_opened_) {
        serve_gate_.done();
    }
}

common::IoResult<void> Http3Endpoint::on_start(Server &server) noexcept {
    if (!options_.tls.enabled()) {
        return std::unexpected(common::IoErr::Invalid); // HTTP/3 is TLS-only
    }
    FIBER_ASSERT(std::all_of(workers_.begin(), workers_.end(), [](auto *worker) { return worker == nullptr; }));
    workers_.clear();
    pending_workers_.clear();
    if (gate_opened_) {
        serve_gate_.add();
        gate_opened_ = false;
    }

    const HttpHandler &handler = options_.handler ? options_.handler : server.default_handler();
    handler_ = std::make_shared<const HttpHandler>(handler);
    if (!handler_) {
        return std::unexpected(common::IoErr::NoMem);
    }
    tls_param_ = make_http3_server_tls_param(options_.tls);

    net::SocketAddress addr = options_.address;
    if (addr.port() == 0 && options_.inherit_port_from != nullptr) {
        const std::uint16_t inherited = options_.inherit_port_from->local_addr().port();
        // Endpoints start in insertion order, so the target must already be bound.
        FIBER_ASSERT_MSG(inherited != 0, "inherit_port_from endpoint has not been started yet");
        addr = net::SocketAddress(addr.ip(), inherited);
    }

    const std::size_t count = server.worker_count();
    const bool reuse_port = count > 1;
    pending_workers_.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        auto worker = std::unique_ptr<Http3EndpointWorker>(
                new (std::nothrow) Http3EndpointWorker(server.worker_loop(index), handler_, options_.http3));
        if (!worker) {
            pending_workers_.clear();
            return std::unexpected(common::IoErr::NoMem);
        }

        auto initialized = worker->init(addr, reuse_port, tls_param_);
        if (!initialized) {
            pending_workers_.clear();
            return std::unexpected(initialized.error());
        }
        if (index == 0) {
            // Resolve the kernel-assigned port once, then bind the rest to it.
            local_addr_ = worker->local_addr();
            addr = local_addr_;
        }
        pending_workers_.push_back(std::move(worker));
    }
    return {};
}

EndpointWorker *Http3Endpoint::create_worker(event::EventLoop &loop, std::size_t index) noexcept {
    if (index >= pending_workers_.size() || !pending_workers_[index]) {
        return nullptr;
    }
    FIBER_ASSERT(&pending_workers_[index]->loop() == &loop);
    Http3EndpointWorker *worker = pending_workers_[index].release();
    workers_.push_back(worker);
    worker->attach(*this, index);
    return worker;
}

async::Task<void> Http3Endpoint::on_serve() noexcept {
    // No accept loop: QUIC admission runs inside each shard's receive path.
    // Kick the shards off on their own loops and then wait for on_stop().
    if (gate_opened_) {
        co_return;
    }
    for (Http3EndpointWorker *worker: workers_) {
        worker->post_start();
    }

    co_await serve_gate_.join();
    co_return;
}

void Http3Endpoint::on_stop() noexcept {
    if (gate_opened_) {
        return;
    }
    pending_workers_.clear();
    gate_opened_ = true;
    serve_gate_.done();
}

} // namespace fiber::http
