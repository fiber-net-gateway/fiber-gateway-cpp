#include "AiServer.h"

#include <cstdint>
#include <string_view>

#include "http/HttpExchange.h"
#include "http/HttpExchangeIo.h"
#include "http/HttpHeaders.h"

namespace fiber::ai_server {

namespace {

constexpr std::string_view kHealthPath = "/health";
constexpr std::string_view kHealthBody = "{\"status\":\"ok\"}\n";
constexpr std::string_view kMethodNotAllowedBody = "{\"error\":\"method_not_allowed\"}\n";
constexpr std::string_view kNotFoundBody = "{\"error\":\"not_found\"}\n";

http::HttpServerOptions make_server_options() noexcept {
    http::HttpServerOptions options;
    options.drain_unread_body = true;
    return options;
}

async::Task<void> send_json(http::HttpExchange &exchange, int status_code, std::string_view body,
                            bool allow_get = false) {
    http::HttpHeaders headers(exchange.pool());
    headers.set_view("Content-Type", "application/json");
    if (allow_get) {
        headers.set_view("Allow", "GET");
    }

    auto header_result = co_await exchange.send_header({
            .kind = http::OutgoingHeaderKind::Final,
            .status_code = status_code,
            .headers = &headers,
            .body = http::HttpBodySpec::ContentLength(body.size()),
            .connection_mode = http::ResponseConnectionMode::Auto,
            .end_stream = body.empty(),
    });
    if (!header_result || body.empty()) {
        co_return;
    }

    (void) co_await exchange.write_body(reinterpret_cast<const std::uint8_t *>(body.data()), body.size(), true);
}

} // namespace

AiServer::AiServer(event::EventLoop &loop) :
    server_(loop, [this](http::HttpExchange &exchange) { return handle(exchange); }, make_server_options()) {}

common::IoResult<void> AiServer::bind(const net::SocketAddress &address, const net::ListenOptions &options) {
    return server_.bind(address, options);
}

async::DetachedTask AiServer::serve() { return server_.serve(); }

void AiServer::close() { server_.close(); }

async::Task<void> AiServer::shutdown_and_wait() { co_await server_.shutdown_and_wait(); }

int AiServer::fd() const noexcept { return server_.fd(); }

async::Task<void> AiServer::handle(http::HttpExchange &exchange) {
    if (exchange.uri().path != kHealthPath) {
        co_await send_json(exchange, 404, kNotFoundBody);
        co_return;
    }
    if (exchange.method() != http::HttpMethod::Get) {
        co_await send_json(exchange, 405, kMethodNotAllowedBody, true);
        co_return;
    }

    co_await send_json(exchange, 200, kHealthBody);
}

} // namespace fiber::ai_server
