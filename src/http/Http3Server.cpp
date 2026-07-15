#include "Http3Server.h"

#include <expected>
#include <new>
#include <utility>

#include "../async/Spawn.h"
#include "../async/WaitGroup.h"
#include "../common/Assert.h"
#include "ServerHttp3Request.h"
#include "TlsAlpn.h"

namespace fiber::http {

class Http3Server::ServerConnection final : public common::NonCopyable, public common::NonMovable {
public:
    ServerConnection(const quic::QuicConnection::Options &quic_options, const HttpHandler &handler,
                     const HttpServerOptions &http_options) noexcept :
        handler_(handler), http_options_(http_options), quic_(make_quic_options(quic_options, this)),
        h3_(quic_, make_http3_options(this)) {}

    [[nodiscard]] quic::QuicConnection &quic() noexcept { return quic_; }

    void start() noexcept {
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

        co_await server_conn->tasks_.join();
        co_await server_conn->h3_.wait_closed();
        delete server_conn;
        co_return;
    }

    HttpHandler handler_;
    HttpServerOptions http_options_;
    quic::QuicConnection quic_;
    Http3Connection h3_;
    async::WaitGroup tasks_{};
    bool cleanup_started_ = false;
};

Http3Server::Http3Server(event::EventLoop &loop, HttpHandler handler, HttpServerOptions options,
                         event::EventLoopGroup *worker_group) :
    loop_(loop), worker_group_(worker_group), handler_(std::move(handler)), options_(std::move(options)) {}

Http3Server::~Http3Server() { close(); }

common::IoResult<void> Http3Server::bind(const net::SocketAddress &addr) noexcept {
    if (initialized_ || !options_.http3.enabled || !options_.tls.enabled) {
        return std::unexpected(common::IoErr::Invalid);
    }

    net::TlsOptions tls_options = options_.tls;
    normalize_http3_alpn(tls_options);
    auto tls_ctx = std::make_unique<net::TlsServerContext>(std::move(tls_options));
    if (!tls_ctx) {
        return std::unexpected(common::IoErr::NoMem);
    }
    auto tls_initialized = tls_ctx->init();
    if (!tls_initialized) {
        return std::unexpected(tls_initialized.error());
    }
    tls_ctx_ = std::move(tls_ctx);

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

        quic::QuicUdpEndpoint::Options endpoint_options = make_endpoint_options(addr, reuse_port);
        auto initialized = shard->endpoint.init(*shard->loop, endpoint_options);
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
    if (!initialized_ || started_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    for (auto &shard: shards_) {
        if (!shard || shard->recv_started || shard->loop == nullptr) {
            continue;
        }
        shard->recv_started = true;
        async::spawn(*shard->loop,
                     [endpoint = &shard->endpoint]() -> async::DetachedTask { co_await endpoint->recv_loop(); });
    }
}

void Http3Server::close() noexcept {
    for (auto &shard: shards_) {
        if (!shard || shard->loop == nullptr) {
            continue;
        }
        if (shard->loop->in_loop()) {
            on_close_shard(shard.get());
            continue;
        }
        if (shard->close_posted) {
            continue;
        }
        shard->close_posted = true;
        shard->loop->post<Shard, &Shard::close_entry, &Http3Server::on_close_shard>(*shard);
    }
}

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
    auto *connection = new (std::nothrow) ServerConnection(options, handler_, options_);
    if (connection == nullptr) {
        return {};
    }

    connection->start();
    return quic::QuicConnection::Lease::adopt(&connection->quic());
}

quic::QuicUdpEndpoint::Options Http3Server::make_endpoint_options(const net::SocketAddress &addr,
                                                                  bool reuse_port) noexcept {
    quic::QuicUdpEndpoint::Options options{};
    options.bind_addr = addr;
    options.max_connections = options_.http3.max_connections_per_shard;
    options.tls_context = tls_ctx_.get();
    options.udp = options_.http3.udp;
    options.udp.reuse_addr = true;
    options.udp.reuse_port = options.udp.reuse_port || reuse_port;
    options.send = options_.http3.send;
    options.transport = options_.http3.transport;
    options.keepalive_interval = options_.http3.keepalive_interval;
    options.recv_flow = options_.http3.recv_flow;
    options.max_ack_delay = options_.http3.max_ack_delay;
    options.ack_delay_exponent = options_.http3.ack_delay_exponent;
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

void Http3Server::on_close_shard(Shard *shard) noexcept {
    if (shard == nullptr) {
        return;
    }
    shard->close_posted = false;
    shard->endpoint.close();
}

} // namespace fiber::http
