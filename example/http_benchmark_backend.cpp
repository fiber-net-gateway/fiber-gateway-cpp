#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <signal.h>
#include <string_view>
#include <sys/socket.h>
#include <vector>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/async/Task.h>
#include <fiber/common/IoError.h>
#include <fiber/event/EventLoop.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/HttpExchange.h>
#include <fiber/http/HttpServer.h>
#include <fiber/net/IpAddress.h>
#include <fiber/net/SocketAddress.h>

namespace {

constexpr std::size_t kBody1kSize = 1024;
constexpr std::size_t kBody64kSize = 64 * 1024;
constexpr std::size_t kBody1mSize = 1024 * 1024;

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

const std::array<std::uint8_t, kBody1mSize> kBody1m = [] {
    std::array<std::uint8_t, kBody1mSize> body{};
    body.fill(static_cast<std::uint8_t>('c'));
    return body;
}();

std::optional<std::uint16_t> parse_port(const char *text) noexcept {
    if (text == nullptr) {
        return std::nullopt;
    }
    char *end = nullptr;
    unsigned long value = std::strtoul(text, &end, 10);
    if (end == nullptr || *end != '\0' || value > 65535) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(value);
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
    (void) co_await exchange.write_all(body, body_size, true);
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
        bool complete = read_result->complete();
        auto write_result = co_await exchange.write_all(std::move(*read_result));
        if (!write_result || complete) {
            co_return;
        }
    }
}

fiber::async::Task<void> discard_body(fiber::http::HttpExchange &exchange) {
    auto discard_result = co_await exchange.discard_body();
    if (!discard_result) {
        exchange.abort(discard_result.error());
        co_return;
    }
    (void) co_await send_header(exchange, 204, nullptr, fiber::http::HttpBodySpec::None(), true);
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
    if (path == "/bench/1m") {
        co_await send_fixed(exchange, kBody1m.data(), kBody1m.size());
        co_return;
    }
    if (path == "/bench/echo") {
        co_await echo_body(exchange);
        co_return;
    }
    if (path == "/bench/discard") {
        co_await discard_body(exchange);
        co_return;
    }
    if (path == "/fault/delay") {
        co_await fiber::async::sleep(std::chrono::milliseconds(250));
        co_await send_fixed(exchange, kBody1k.data(), kBody1k.size());
        co_return;
    }
    if (path == "/fault/hang") {
        co_await fiber::async::sleep(std::chrono::seconds(60));
        co_await send_fixed(exchange, nullptr, 0);
        co_return;
    }
    if (path == "/fault/close") {
        exchange.abort(fiber::common::IoErr::Canceled);
        co_return;
    }
    if (path == "/fault/partial") {
        auto header_result = co_await send_header(exchange, 200, nullptr,
                                                  fiber::http::HttpBodySpec::ContentLength(kBody64k.size()), false);
        if (header_result) {
            (void) co_await exchange.write_all(kBody1k.data(), kBody1k.size(), false);
        }
        exchange.abort(fiber::common::IoErr::Canceled);
        co_return;
    }

    static constexpr std::string_view kNotFound = "404 Not Found\n";
    auto header_result = co_await send_header(exchange, 404, nullptr,
                                              fiber::http::HttpBodySpec::ContentLength(kNotFound.size()), false);
    if (header_result) {
        (void) co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>(kNotFound.data()), kNotFound.size(),
                                           true);
    }
}

} // namespace

int main(int argc, char **argv) {
    std::uint16_t port = 19001;
    if (argc > 1) {
        auto parsed_port = parse_port(argv[1]);
        if (!parsed_port) {
            std::cerr << "usage: http_benchmark_backend [port]\n";
            return 1;
        }
        port = *parsed_port;
    }

    (void) ::signal(SIGPIPE, SIG_IGN);

    fiber::event::EventLoop accept_loop;
    fiber::event::EventLoopGroup worker_group(4);
    worker_group.start();

    fiber::http::HttpServerOptions server_options{};
    server_options.drain_unread_body = true;
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

    std::cout << "benchmark backend listening on 127.0.0.1:" << port << '\n';
    fiber::async::spawn(accept_loop, [&server]() { return server.serve(); });
    accept_loop.run();

    server.close();
    worker_group.stop();
    worker_group.join();
    return 0;
}
