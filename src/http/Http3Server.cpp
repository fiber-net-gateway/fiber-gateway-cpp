#include <fiber/http/Http3Server.h>

#include <expected>
#include <new>
#include <utility>

#include <fiber/async/Spawn.h>
#include <fiber/async/WaitGroup.h>
#include <fiber/common/Assert.h>
#include "http/ServerHttp3Request.h"
#include "http/TlsAlpn.h"

namespace fiber::http {

struct Http3Server::Runtime {
    explicit Runtime(HttpHandler handler) : handler(std::make_shared<HttpHandler>(std::move(handler))) {}

    std::shared_ptr<const HttpHandler> handler{};
    std::atomic<bool> shutting_down{false};
    async::WaitGroup connections{};
};

class Http3Server::ServerConnection final : public common::NonCopyable, public common::NonMovable {
public:
    ServerConnection(const quic::QuicConnection::Options &quic_options, std::shared_ptr<const HttpHandler> handler,
                     const HttpServerOptions &http_options, std::shared_ptr<Runtime> runtime) noexcept :
        runtime_(std::move(runtime)), handler_(std::move(handler)), http_options_(http_options),
        quic_(make_quic_options(quic_options, this)), h3_(quic_, make_http3_options(this)) {
        prepared_ = h3_.prepare().has_value();
    }

    [[nodiscard]] quic::QuicConnection &quic() noexcept { return quic_; }

    void start() noexcept {
        if (!prepared_) {
            h3_.close(Http3ErrorCode::InternalError);
            return;
        }
        event::EventLoop *loop = quic_.loop();
        FIBER_ASSERT(loop != nullptr);
        tasks_.add();
        async::spawn(*loop, [this]() -> async::DetachedTask { return run_start(this); });
    }

private:
    [[nodiscard]] static quic::QuicConnection::Options make_quic_options(const quic::QuicConnection::Options &base,
                                                                         ServerConnection *owner) noexcept {
        quic::QuicConnection::Options options = base;
        options.destroy_owner = owner;
        options.on_destroy = &ServerConnection::destroy_connection;
        return options;
    }

    [[nodiscard]] static Http3Connection::Options make_http3_options(ServerConnection *owner) noexcept {
        Http3Connection::Options options{};
        options.local_settings = owner->http_options_.http3.settings;
        options.local_settings.enable_connect_protocol =
                options.local_settings.enable_connect_protocol || owner->http_options_.enable_extended_connect;
        options.owner = owner;
        options.ops.create_server_request = &ServerConnection::create_server_request;
        return options;
    }

    [[nodiscard]] static quic::QuicStream::Lease create_server_request(void *owner, std::uint64_t stream_id,
                                                                       Http3Connection &conn) noexcept {
        auto *server_conn = static_cast<ServerConnection *>(owner);
        if (server_conn == nullptr) {
            return {};
        }
        return ServerHttp3Request::create(stream_id, conn, server_conn->http_options_, server_conn->handler_);
    }

    static void destroy_connection(void *owner, quic::QuicConnection &connection) noexcept {
        auto *server_conn = static_cast<ServerConnection *>(owner);
        if (server_conn == nullptr || server_conn->cleanup_started_) {
            return;
        }

        server_conn->cleanup_started_ = true;
        event::EventLoop *loop = connection.loop();
        if (loop == nullptr) {
            if (server_conn->runtime_) {
                server_conn->runtime_->connections.done();
            }
            delete server_conn;
            return;
        }

        async::spawn(*loop, [server_conn]() -> async::DetachedTask { return run_cleanup(server_conn); });
    }

    static async::DetachedTask run_start(ServerConnection *server_conn) noexcept {
        if (server_conn == nullptr) {
            co_return;
        }

        auto started = co_await server_conn->h3_.start();
        if (!started && started.error() != common::IoErr::Canceled) {
            server_conn->h3_.close(Http3ErrorCode::InternalError);
        }
        server_conn->tasks_.done();
        co_return;
    }

    static async::DetachedTask run_cleanup(ServerConnection *server_conn) noexcept {
        if (server_conn == nullptr) {
            co_return;
        }

        std::shared_ptr<Runtime> runtime = server_conn->runtime_;
        co_await server_conn->tasks_.join();
        co_await server_conn->h3_.wait_closed();
        if (runtime) {
            runtime->connections.done();
        }
        delete server_conn;
        co_return;
    }

    std::shared_ptr<Runtime> runtime_;
    std::shared_ptr<const HttpHandler> handler_{};
    HttpServerOptions http_options_;
    quic::QuicConnection quic_;
    Http3Connection h3_;
    async::WaitGroup tasks_{};
    bool cleanup_started_ = false;
    bool prepared_ = false;
};

Http3Server::Http3Server(event::EventLoop &loop, HttpHandler handler, HttpServerOptions options,
                         event::EventLoopGroup *worker_group) :
    loop_(loop), worker_group_(worker_group), runtime_(std::make_shared<Runtime>(std::move(handler))),
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
    auto *connection = new (std::nothrow) ServerConnection(options, runtime_->handler, options_, runtime_);
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
