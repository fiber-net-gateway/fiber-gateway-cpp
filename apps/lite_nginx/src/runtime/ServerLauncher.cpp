#include "ServerLauncher.h"

#include <cerrno>
#include <cstring>
#include <utility>
#include <string_view>
#include <sys/socket.h>

#include "async/Spawn.h"
#include "async/Task.h"
#include "common/IoError.h"
#include "http/HttpExchange.h"
#include "http/HttpHeaders.h"
#include "http/TlsContext.h"
#include "net/IpAddress.h"
#include "net/TcpListener.h"

namespace fiber::lite_nginx::runtime {
namespace {

constexpr std::string_view kHelloBody = "hello lite nginx\n";

RuntimeError make_error(const config::SourceLocation &location, std::string message) {
    return RuntimeError{
        .message = std::move(message),
        .location = location,
    };
}

fiber::async::Task<fiber::common::IoResult<void>> send_final_header(
    fiber::http::HttpExchange &exchange,
    int status_code,
    const fiber::http::HttpHeaders *headers,
    std::size_t content_length,
    bool end_stream) {
    co_return co_await exchange.send_header({
        .kind = fiber::http::OutgoingHeaderKind::Final,
        .status_code = status_code,
        .headers = headers,
        .body_mode = fiber::http::ResponseBodyMode::ContentLength,
        .connection_mode = fiber::http::ResponseConnectionMode::Auto,
        .content_length = content_length,
        .end_stream = end_stream,
    });
}

fiber::async::Task<void> handle_hello(fiber::http::HttpExchange &exchange) {
    fiber::http::HttpHeaders headers(exchange.pool());
    headers.set("Content-Type", "text/plain");

    auto header_result = co_await send_final_header(exchange, 200, &headers, kHelloBody.size(), false);
    if (!header_result) {
        co_return;
    }

    auto body_result = co_await exchange.write_body(
        reinterpret_cast<const std::uint8_t *>(kHelloBody.data()), kHelloBody.size(), true);
    (void)body_result;
    co_return;
}

fiber::http::TlsContext *select_identity_by_server_name(void *, const fiber::http::TlsClientHelloView &client_hello) noexcept {
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
        return std::unexpected(make_error(
            listener.location,
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
    for (const auto &identity : listener.tls_identities) {
        options.tls.identities.push_back({
            .name = identity.server_name,
            .cert_file = identity.certificate,
            .key_file = identity.certificate_key,
        });
    }
    return options;
}

} // namespace

ServerLauncher::ServerLauncher(fiber::event::EventLoop &accept_loop) : accept_loop_(accept_loop) {}

ServerLauncher::~ServerLauncher() { close(); }

std::expected<void, RuntimeError> ServerLauncher::start(const RuntimeConfig &runtime) {
    if (started_) {
        return std::unexpected(make_error({}, "lite-nginx runtime already started"));
    }

    if (runtime.worker_processes > 1) {
        worker_group_ = std::make_unique<fiber::event::EventLoopGroup>(runtime.worker_processes);
        worker_group_->start();
    }

    servers_.reserve(runtime.listeners.size());
    bound_listeners_.reserve(runtime.listeners.size());

    for (const auto &listener : runtime.listeners) {
        auto addr_result = make_socket_address(listener);
        if (!addr_result) {
            close();
            return std::unexpected(addr_result.error());
        }

        auto options = make_server_options(listener);
        auto server = std::make_unique<fiber::http::HttpServer>(accept_loop_, handle_hello, std::move(options),
                                                                worker_group_.get());

        fiber::net::ListenOptions listen_options{};
        auto bind_result = server->bind(*addr_result, listen_options);
        if (!bind_result) {
            close();
            return std::unexpected(make_error(
                listener.location,
                "bind failed for listen " + addr_result->to_string() + ": " +
                    std::string(fiber::common::io_err_name(bind_result.error()))));
        }

        auto bound_port_result = resolve_port(server->fd());
        if (!bound_port_result) {
            close();
            return std::unexpected(make_error(
                listener.location,
                "failed to resolve bound port for listen " + addr_result->to_string()));
        }

        fiber::net::SocketAddress bound_address(
            addr_result->ip(),
            *bound_port_result);
        bound_listeners_.push_back({
            .address = bound_address,
            .tls = listener.tls,
        });

        auto *server_ptr = server.get();
        fiber::async::spawn(accept_loop_, [server_ptr]() { return server_ptr->serve(); });
        servers_.push_back(std::move(server));
    }

    started_ = true;
    return {};
}

void ServerLauncher::close() {
    for (auto &server : servers_) {
        server->close();
    }
    servers_.clear();
    bound_listeners_.clear();
    started_ = false;

    if (worker_group_) {
        worker_group_->stop();
        worker_group_->join();
        worker_group_.reset();
    }
}

} // namespace fiber::lite_nginx::runtime
