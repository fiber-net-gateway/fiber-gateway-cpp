#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <signal.h>
#include <string_view>
#include <utility>

#include "async/Spawn.h"
#include "async/Task.h"
#include "common/IoError.h"
#include "event/EventLoop.h"
#include "event/EventLoopGroup.h"
#include "http/HttpExchange.h"
#include "http/HttpServer.h"
#include "net/IpAddress.h"
#include "net/SocketAddress.h"

namespace {

constexpr std::size_t kBody1kSize = 1024;
constexpr std::size_t kBody64kSize = 64 * 1024;

const std::array<std::uint8_t, kBody1kSize> kBody1k = [] {
    std::array<std::uint8_t, kBody1kSize> body{};
    body.fill(static_cast<std::uint8_t>('a'));
    return body;
}();

const std::array<std::uint8_t, kBody64kSize> kBody64k = [] {
    std::array<std::uint8_t, kBody64kSize> body{};
    body.fill(static_cast<std::uint8_t>('b'));
    return body;
}();

std::optional<std::size_t> parse_size(const char *text, std::size_t maximum) noexcept {
    if (text == nullptr) {
        return std::nullopt;
    }
    char *end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 10);
    if (end == nullptr || *end != '\0' || value == 0 || value > maximum) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(value);
}

fiber::async::Task<fiber::common::IoResult<void>> send_header(fiber::http::HttpExchange &exchange, int status,
                                                              const fiber::http::HttpHeaders *headers,
                                                              fiber::http::HttpBodySpec body, bool end_stream) {
    co_return co_await exchange.send_header({
            .kind = fiber::http::OutgoingHeaderKind::Final,
            .status_code = status,
            .headers = headers,
            .body = body,
            .end_stream = end_stream,
    });
}

fiber::async::Task<void> send_fixed(fiber::http::HttpExchange &exchange, const std::uint8_t *body,
                                    std::size_t body_size) {
    fiber::http::HttpHeaders headers(exchange.pool());
    headers.set("Content-Type", "application/octet-stream");
    auto header_result = co_await send_header(exchange, 200, &headers,
                                              fiber::http::HttpBodySpec::ContentLength(body_size), body_size == 0);
    if (!header_result || body_size == 0) {
        co_return;
    }
    (void) co_await exchange.write_body(body, body_size, true);
}

fiber::async::Task<void> echo_body(fiber::http::HttpExchange &exchange) {
    const auto request_body = exchange.request_body_spec();
    const auto response_body = request_body.is_content_length()
                                       ? fiber::http::HttpBodySpec::ContentLength(request_body.content_length())
                                       : fiber::http::HttpBodySpec::Auto();

    fiber::http::HttpHeaders headers(exchange.pool());
    headers.set("Content-Type", "application/octet-stream");
    auto header_result = co_await send_header(exchange, 200, &headers, response_body, false);
    if (!header_result) {
        co_return;
    }

    for (;;) {
        auto read_result = co_await exchange.read_body(64 * 1024);
        if (!read_result) {
            exchange.abort(read_result.error());
            co_return;
        }
        const bool complete = read_result->complete();
        auto write_result = co_await exchange.write_body(std::move(*read_result));
        if (!write_result || complete) {
            co_return;
        }
    }
}

fiber::async::Task<void> handle_request(fiber::http::HttpExchange &exchange) {
    const std::string_view path = exchange.uri().path;
    if (path == "/bench/1k") {
        co_await send_fixed(exchange, kBody1k.data(), kBody1k.size());
        co_return;
    }
    if (path == "/bench/64k") {
        co_await send_fixed(exchange, kBody64k.data(), kBody64k.size());
        co_return;
    }
    if (path == "/bench/echo") {
        co_await echo_body(exchange);
        co_return;
    }

    static constexpr std::string_view kNotFound = "404 Not Found\n";
    auto header_result = co_await send_header(exchange, 404, nullptr,
                                              fiber::http::HttpBodySpec::ContentLength(kNotFound.size()), false);
    if (header_result) {
        (void) co_await exchange.write_body(reinterpret_cast<const std::uint8_t *>(kNotFound.data()), kNotFound.size(),
                                            true);
    }
}

} // namespace

int main(int argc, char **argv) {
    std::uint16_t port = 18443;
    std::size_t workers = 2;
    const char *cert_file = "build/http3-demo/cert.pem";
    const char *key_file = "build/http3-demo/key.pem";
    if (argc > 1) {
        auto parsed = parse_size(argv[1], 65535);
        if (!parsed) {
            std::cerr << "invalid port\n";
            return 1;
        }
        port = static_cast<std::uint16_t>(*parsed);
    }
    if (argc > 2) {
        auto parsed = parse_size(argv[2], 64);
        if (!parsed) {
            std::cerr << "invalid worker count\n";
            return 1;
        }
        workers = *parsed;
    }
    if (argc > 3) {
        cert_file = argv[3];
    }
    if (argc > 4) {
        key_file = argv[4];
    }
    if (argc > 5) {
        std::cerr << "usage: http3_benchmark_server [port] [workers] [cert] [key]\n";
        return 1;
    }

    (void) ::signal(SIGPIPE, SIG_IGN);

    fiber::event::EventLoop accept_loop;
    fiber::event::EventLoopGroup worker_group(workers);
    worker_group.start();

    fiber::http::HttpServerOptions server_options{};
    server_options.drain_unread_body = true;
    server_options.tls.enabled = true;
    server_options.tls.cert_file = cert_file;
    server_options.tls.key_file = key_file;
    server_options.tls.alpn = {"h2", "http/1.1"};
    server_options.http3.enabled = true;
    fiber::http::HttpServer server(accept_loop, handle_request, server_options, &worker_group);
    fiber::net::ListenOptions listen_options{};
    fiber::net::SocketAddress address(fiber::net::IpAddress::loopback_v4(), port);
    auto bind_result = server.bind(address, listen_options);
    if (!bind_result) {
        std::cerr << "bind failed: " << fiber::common::io_err_name(bind_result.error()) << '\n';
        worker_group.stop();
        worker_group.join();
        return 1;
    }

    std::cout << "HTTP/3 benchmark server listening on 127.0.0.1:" << port << " workers=" << workers << '\n';
    fiber::async::spawn(accept_loop, [&server]() { return server.serve(); });
    accept_loop.run();

    server.close();
    worker_group.stop();
    worker_group.join();
    return 0;
}
