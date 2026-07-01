#include "ServerLauncher.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>
#include <string_view>
#include <sys/socket.h>
#include <utility>

#include "async/Spawn.h"
#include "async/Task.h"
#include "common/IoError.h"
#include "http/HttpExchange.h"
#include "http/HttpExchangeIo.h"
#include "http/HttpHeaders.h"
#include "net/IpAddress.h"
#include "net/TcpListener.h"
#include "net/TlsContext.h"

#include "../proxy/ProxyHandler.h"
#include "../upstream/UpstreamRegistry.h"

namespace fiber::lite_nginx::runtime {
namespace {

constexpr std::string_view kNotFoundBody = "404 Not Found\n";

RuntimeError make_error(const config::SourceLocation &location, std::string message) {
    return RuntimeError{
            .message = std::move(message),
            .location = location,
    };
}

fiber::async::Task<void> send_plain_response(fiber::http::HttpExchange &exchange, int status_code,
                                             std::string_view body, const ListenerRuntime *listener = nullptr) {
    fiber::http::HttpHeaders headers(exchange.pool());
    headers.set("Content-Type", "text/plain");
    if (listener != nullptr && listener->http3 && !listener->http3_alt_svc.empty()) {
        headers.set("Alt-Svc", listener->http3_alt_svc);
    }

    auto header_result = co_await exchange.send_header({
            .kind = fiber::http::OutgoingHeaderKind::Final,
            .status_code = status_code,
            .headers = &headers,
            .body = fiber::http::HttpBodySpec::ContentLength(body.size()),
            .connection_mode = fiber::http::ResponseConnectionMode::Auto,
            .end_stream = body.empty(),
    });
    if (!header_result) {
        co_return;
    }
    if (body.empty()) {
        co_return;
    }

    (void) co_await exchange.write_body(reinterpret_cast<const std::uint8_t *>(body.data()), body.size(), true);
}

fiber::net::TlsContext *select_identity_by_server_name(void *,
                                                       const fiber::net::TlsClientHelloView &client_hello) noexcept {
    if (!client_hello.server_context || client_hello.server_name.empty()) {
        return nullptr;
    }
    return client_hello.server_context->find_identity_by_name(client_hello.server_name);
}

std::expected<fiber::net::SocketAddress, RuntimeError> make_socket_address(const ListenerRuntime &listener) {
    if (!listener.has_host) {
        return fiber::net::SocketAddress::any_v4(listener.port);
    }

    fiber::net::IpAddress ip;
    if (!fiber::net::IpAddress::parse(listener.host, ip)) {
        return std::unexpected(make_error(listener.location,
                                          "listen host must be an IP literal in lite-nginx runtime: " + listener.host));
    }
    return fiber::net::SocketAddress(ip, listener.port);
}

fiber::common::IoResult<std::uint16_t> resolve_port(int fd) {
    sockaddr_storage bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&bound), &len) != 0) {
        return std::unexpected(fiber::common::io_err_from_errno(errno));
    }
    fiber::net::SocketAddress local;
    if (!fiber::net::SocketAddress::from_sockaddr(reinterpret_cast<sockaddr *>(&bound), len, local)) {
        return std::unexpected(fiber::common::IoErr::NotSupported);
    }
    return local.port();
}

fiber::http::HttpServerOptions make_server_options(const ListenerRuntime &listener) {
    fiber::http::HttpServerOptions options;
    options.drain_unread_body = true;
    options.http3.enabled = listener.http3;
    if (!listener.tls) {
        return options;
    }

    options.tls.enabled = true;
    options.tls.cert_file = listener.default_certificate;
    options.tls.key_file = listener.default_certificate_key;
    options.tls.alpn = {"h2", "http/1.1"};
    options.tls.identity_selector_ops = {
            .select = &select_identity_by_server_name,
            .ctx = nullptr,
    };
    options.tls.identities.reserve(listener.tls_identities.size());
    for (const auto &identity: listener.tls_identities) {
        options.tls.identities.push_back({
                .name = identity.server_name,
                .cert_file = identity.certificate,
                .key_file = identity.certificate_key,
        });
    }
    return options;
}

std::string_view strip_host_port(std::string_view host) noexcept {
    if (host.empty()) {
        return {};
    }
    if (host.front() == '[') {
        const std::size_t end = host.find(']');
        if (end == std::string_view::npos) {
            return host;
        }
        return host.substr(1, end - 1);
    }

    const std::size_t colon = host.find(':');
    if (colon == std::string_view::npos) {
        return host;
    }
    return host.substr(0, colon);
}

std::uint32_t find_server_index(const ListenerRuntime &listener, std::string_view host_name) noexcept {
    if (host_name.empty() || listener.server_names.empty()) {
        return listener.default_server_index;
    }

    auto it = std::lower_bound(listener.server_names.begin(), listener.server_names.end(), host_name,
                               [](const ServerNameRuntime &entry, std::string_view key) { return entry.name < key; });
    if (it == listener.server_names.end() || it->name != host_name) {
        return listener.default_server_index;
    }
    return it->server_index;
}

struct LocationMatchContext {
    static constexpr std::uint32_t kInvalidIndex = std::numeric_limits<std::uint32_t>::max();

    bool matched(std::uint32_t, const std::uint32_t &handler) {
        location_index = handler;
        return true;
    }

    void add_path_var(std::string_view, std::string_view) {}
    void pop_path_var() {}

    std::uint32_t location_index = kInvalidIndex;
};

class RequestDispatcher {
public:
    RequestDispatcher(std::shared_ptr<const RuntimeConfig> runtime,
                      std::shared_ptr<upstream::UpstreamRegistry> upstreams) noexcept :
        runtime_(std::move(runtime)), upstreams_(std::move(upstreams)), proxy_(*upstreams_) {}

    fiber::async::Task<void> handle(std::uint32_t listener_index, fiber::http::HttpExchange &exchange) const {
        if (!runtime_ || listener_index >= runtime_->listeners.size()) {
            co_await send_plain_response(exchange, 404, kNotFoundBody);
            co_return;
        }

        const ListenerRuntime &listener = runtime_->listeners[listener_index];
        std::string_view host_name;
        if (const auto *host = exchange.host_header(); host != nullptr) {
            host_name = strip_host_port(host->value_view());
        }

        const std::uint32_t server_index = find_server_index(listener, host_name);
        if (server_index >= runtime_->servers.size()) {
            co_await send_plain_response(exchange, 404, kNotFoundBody, &listener);
            co_return;
        }

        const ServerRuntime &server = runtime_->servers[server_index];
        LocationMatchContext match_context;
        std::string_view path = exchange.uri().path.empty() ? std::string_view("/") : exchange.uri().path;
        if (!server.location_matcher.match_path(path, match_context) ||
            match_context.location_index >= server.locations.size()) {
            co_await send_plain_response(exchange, 404, kNotFoundBody, &listener);
            co_return;
        }

        co_await proxy_.handle(exchange, listener, server.locations[match_context.location_index]);
    }

private:
    std::shared_ptr<const RuntimeConfig> runtime_;
    std::shared_ptr<upstream::UpstreamRegistry> upstreams_;
    proxy::ProxyHandler proxy_;
};

} // namespace

ServerLauncher::ServerLauncher(fiber::event::EventLoop &accept_loop) : accept_loop_(accept_loop) {}

ServerLauncher::~ServerLauncher() { close(); }

std::expected<void, RuntimeError> ServerLauncher::start(const RuntimeConfig &runtime) {
    if (started_) {
        return std::unexpected(make_error({}, "lite-nginx runtime already started"));
    }

    runtime_ = std::make_shared<RuntimeConfig>(runtime);
    worker_group_ =
            std::make_unique<fiber::event::EventLoopGroup>(std::max<std::size_t>(runtime_->worker_processes, 1));
    worker_group_->start();

    upstreams_ = std::make_shared<upstream::UpstreamRegistry>(*worker_group_, *runtime_);
    if (!upstreams_->init()) {
        close();
        return std::unexpected(make_error({}, "failed to initialize upstream registry"));
    }

    auto dispatcher = std::make_shared<RequestDispatcher>(runtime_, upstreams_);

    servers_.reserve(runtime_->listeners.size());
    bound_listeners_.reserve(runtime_->listeners.size());

    for (std::uint32_t listener_index = 0; listener_index < runtime_->listeners.size(); ++listener_index) {
        const auto &listener = runtime_->listeners[listener_index];
        auto addr_result = make_socket_address(listener);
        if (!addr_result) {
            close();
            return std::unexpected(addr_result.error());
        }

        auto options = make_server_options(listener);
        auto server = std::make_unique<fiber::http::HttpServer>(
                accept_loop_,
                [dispatcher, listener_index](fiber::http::HttpExchange &exchange) {
                    return dispatcher->handle(listener_index, exchange);
                },
                std::move(options), worker_group_.get());

        fiber::net::ListenOptions listen_options{};
        auto bind_result = server->bind(*addr_result, listen_options);
        if (!bind_result) {
            close();
            return std::unexpected(make_error(listener.location,
                                              "bind failed for listen " + addr_result->to_string() + ": " +
                                                      std::string(fiber::common::io_err_name(bind_result.error()))));
        }

        auto bound_port_result = resolve_port(server->fd());
        if (!bound_port_result) {
            close();
            return std::unexpected(make_error(listener.location,
                                              "failed to resolve bound port for listen " + addr_result->to_string()));
        }

        fiber::net::SocketAddress bound_address(addr_result->ip(), *bound_port_result);
        bound_listeners_.push_back({
                .address = bound_address,
                .tls = listener.tls,
                .http3 = listener.http3,
        });

        auto *server_ptr = server.get();
        fiber::async::spawn(accept_loop_, [server_ptr]() { return server_ptr->serve(); });
        servers_.push_back(std::move(server));
    }

    started_ = true;
    return {};
}

void ServerLauncher::close() {
    for (auto &server: servers_) {
        server->close();
    }
    servers_.clear();
    bound_listeners_.clear();
    started_ = false;

    if (upstreams_) {
        upstreams_->shutdown();
    }

    if (worker_group_) {
        worker_group_->stop();
        worker_group_->join();
    }

    upstreams_.reset();
    runtime_.reset();
    worker_group_.reset();
}

} // namespace fiber::lite_nginx::runtime
