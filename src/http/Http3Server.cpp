#include <fiber/http/Http3Server.h>

#include <expected>
#include <new>
#include <utility>

#include <fiber/async/Spawn.h>
#include <fiber/async/WaitGroup.h>
#include <fiber/common/Assert.h>
#include "http/Http3ServerConnection.h"
#include "http/TlsAlpn.h"

namespace fiber::http {

namespace {

// The facade still carries the catch-all HttpServerOptions; HTTP/3 sessions now
// take only the fields they read. Both go away with the facade (P5).
Http3ServerOptions make_http3_options(const HttpServerOptions &options) noexcept {
    Http3ServerOptions http3{};
    http3.max_connections_per_shard = options.http3.max_connections_per_shard;
    http3.retained_storage_limit = options.http3.retained_storage_limit;
    http3.udp = options.http3.udp;
    http3.send = options.http3.send;
    http3.transport = options.http3.transport;
    http3.recv_flow = options.http3.recv_flow;
    http3.settings = options.http3.settings;
    http3.keepalive_interval = options.http3.keepalive_interval;
    http3.max_ack_delay = options.http3.max_ack_delay;
    http3.body_timeout = options.body_timeout;
    http3.header_large_size = options.header_large_size;
    http3.ack_delay_exponent = options.http3.ack_delay_exponent;
    http3.enable_connect_protocol = options.enable_extended_connect;
    http3.retry = options.http3.retry;
    http3.issue_new_token = options.http3.issue_new_token;
    http3.enable_early_data = options.http3.enable_early_data;
    return http3;
}

} // namespace

struct Http3Server::Runtime {
    explicit Runtime(HttpHandler handler, const HttpServerOptions &options) :
        handler(std::make_shared<HttpHandler>(std::move(handler))), http3_options(make_http3_options(options)) {}

    static void on_connection_closed(void *owner, Http3ServerConnection &) noexcept {
        static_cast<Runtime *>(owner)->connections.done();
    }

    static const Http3ServerConnection::Ops &connection_ops() noexcept {
        static const Http3ServerConnection::Ops ops{&Runtime::on_connection_closed};
        return ops;
    }

    std::shared_ptr<const HttpHandler> handler{};
    Http3ServerOptions http3_options{};
    std::atomic<bool> shutting_down{false};
    async::WaitGroup connections{};
};

Http3Server::Http3Server(event::EventLoop &loop, HttpHandler handler, HttpServerOptions options,
                         event::EventLoopGroup *worker_group) :
    loop_(loop), worker_group_(worker_group), runtime_(std::make_shared<Runtime>(std::move(handler), options)),
    options_(std::move(options)) {}

Http3Server::~Http3Server() { close(); }

common::IoResult<void> Http3Server::bind(const net::SocketAddress &addr) noexcept {
    if (initialized_ || !options_.http3.enabled || !options_.tls.enabled()) {
        return std::unexpected(common::IoErr::Invalid);
    }

    quic_tls_param_ = make_http3_server_tls_param(options_.tls);

    const std::size_t count = shard_count();
    shards_.reserve(count);
    const bool reuse_port = count > 1;
    for (std::size_t i = 0; i < count; ++i) {
        auto shard = std::make_unique<Shard>();
        if (!shard) {
            close();
            return std::unexpected(common::IoErr::NoMem);
        }

        shard->server = this;
        shard->loop = worker_group_ != nullptr ? &worker_group_->at(i) : &loop_;

        quic::QuicUdpEndpoint::EndpointOptions endpoint_options = make_endpoint_options(addr, reuse_port);
        quic::QuicUdpEndpoint::ServerAdmissionOptions server_options = make_server_admission_options();
        auto initialized = shard->endpoint.init(*shard->loop, endpoint_options, server_options);
        if (!initialized) {
            close();
            return std::unexpected(initialized.error());
        }

        if (i == 0) {
            local_addr_ = shard->endpoint.local_addr();
        }
        shards_.push_back(std::move(shard));
    }

    initialized_ = true;
    return {};
}

void Http3Server::serve() noexcept {
    if (!initialized_ || runtime_->shutting_down.load(std::memory_order_acquire) ||
        started_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    for (auto &shard: shards_) {
        if (!shard || shard->endpoint_started || shard->loop == nullptr) {
            continue;
        }
        shard->endpoint_started = true;
        if (shard->loop->in_loop()) {
            on_start_shard(shard.get());
        } else {
            shard->loop->post<Shard, &Shard::start_entry, &Http3Server::on_start_shard>(*shard);
        }
    }
}

void Http3Server::close() noexcept {
    runtime_->shutting_down.store(true, std::memory_order_release);
    for (auto &shard: shards_) {
        if (!shard || shard->loop == nullptr) {
            continue;
        }
        if (shard->close_completed || shard->close_posted) {
            continue;
        }
        shard->close_posted = true;
        close_wg_.add();
        if (shard->loop->in_loop()) {
            on_close_shard(shard.get());
        } else {
            shard->loop->post<Shard, &Shard::close_entry, &Http3Server::on_close_shard>(*shard);
        }
    }
}

fiber::async::Task<void> Http3Server::shutdown_and_wait() noexcept {
    runtime_->shutting_down.store(true, std::memory_order_release);
    close();
    co_await close_wg_.join();
    co_await runtime_->connections.join();
    co_return;
}

bool Http3Server::shutting_down() const noexcept { return runtime_->shutting_down.load(std::memory_order_acquire); }

bool Http3Server::valid() const noexcept {
    for (const auto &shard: shards_) {
        if (shard && shard->endpoint.valid()) {
            return true;
        }
    }
    return false;
}

const net::SocketAddress &Http3Server::local_addr() const noexcept { return local_addr_; }

quic::QuicConnection::Lease Http3Server::create_connection_op(void *owner,
                                                              const quic::QuicConnection::Options &options) noexcept {
    auto *server = static_cast<Http3Server *>(owner);
    if (server == nullptr) {
        return {};
    }
    return server->create_connection(options);
}

quic::QuicConnection::Lease Http3Server::create_connection(const quic::QuicConnection::Options &options) noexcept {
    if (runtime_->shutting_down.load(std::memory_order_acquire)) {
        return {};
    }
    runtime_->connections.add();
    Http3ServerConnection *connection = Http3ServerConnection::create(
            options, runtime_->handler, runtime_->http3_options, runtime_.get(), Runtime::connection_ops());
    if (connection == nullptr) {
        runtime_->connections.done();
        return {};
    }

    connection->start();
    return quic::QuicConnection::Lease::adopt(&connection->quic());
}

quic::QuicUdpEndpoint::EndpointOptions Http3Server::make_endpoint_options(const net::SocketAddress &addr,
                                                                          bool reuse_port) noexcept {
    quic::QuicUdpEndpoint::EndpointOptions options{};
    options.bind_addr = addr;
    options.max_connections = options_.http3.max_connections_per_shard;
    options.retained_storage_limit = options_.http3.retained_storage_limit;
    options.udp = options_.http3.udp;
    options.udp.reuse_addr = true;
    options.udp.reuse_port = options.udp.reuse_port || reuse_port;
    options.send = options_.http3.send;
    return options;
}

quic::QuicUdpEndpoint::ServerAdmissionOptions Http3Server::make_server_admission_options() noexcept {
    quic::QuicUdpEndpoint::ServerAdmissionOptions options{};
    options.tls = &quic_tls_param_;
    options.transport = options_.http3.transport;
    options.transport.max_ack_delay = options_.http3.max_ack_delay;
    options.transport.ack_delay_exponent = options_.http3.ack_delay_exponent;
    options.keepalive_interval = options_.http3.keepalive_interval;
    options.recv_flow = options_.http3.recv_flow;
    options.retry = options_.http3.retry;
    options.issue_new_token = options_.http3.issue_new_token;
    options.connection_owner = this;
    options.create_connection = &Http3Server::create_connection_op;
    options.enable_early_data = options_.http3.enable_early_data;
    return options;
}

std::size_t Http3Server::shard_count() const noexcept {
    if (worker_group_ == nullptr || worker_group_->size() == 0) {
        return 1;
    }
    return worker_group_->size();
}

void Http3Server::on_start_shard(Shard *shard) noexcept {
    if (shard == nullptr || !shard->endpoint.valid()) {
        return;
    }
    auto started = shard->endpoint.start();
    if (!started && started.error() != common::IoErr::Already) {
        shard->endpoint.close();
    }
}

void Http3Server::on_close_shard(Shard *shard) noexcept {
    if (shard == nullptr) {
        return;
    }
    shard->close_posted = false;
    shard->close_completed = true;
    shard->endpoint.close();
    // close_wg_ is incremented before either the direct or posted callback.
    // The endpoint close itself is loop-affine and is complete at this point.
    if (shard->server != nullptr) {
        shard->server->close_wg_.done();
    }
}

} // namespace fiber::http
