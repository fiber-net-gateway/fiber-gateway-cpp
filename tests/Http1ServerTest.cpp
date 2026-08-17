#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <coroutine>
#include <cstring>
#include <future>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/async/Task.h>
#include <fiber/common/IoError.h>
#include <fiber/common/mem/IoBuf.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/Http1Server.h>
#include <fiber/net/SocketAddress.h>

namespace {

using fiber::async::DetachedTask;
using namespace std::chrono_literals;

fiber::async::Task<fiber::common::IoResult<void>>
send_final_header(fiber::http::HttpExchange &exchange, int status_code,
                  const fiber::http::HttpHeaders *headers = nullptr,
                  fiber::http::ResponseBodySpec body = fiber::http::ResponseBodySpec::Auto(),
                  fiber::http::ResponseConnectionMode connection_mode = fiber::http::ResponseConnectionMode::Auto,
                  bool end_stream = false) {
    co_return co_await exchange.send_header({
            .kind = fiber::http::OutgoingHeaderKind::Final,
            .status_code = status_code,
            .headers = headers,
            .body = body,
            .connection_mode = connection_mode,
            .end_stream = end_stream,
    });
}

fiber::async::Task<fiber::common::IoResult<void>> send_trailer_header(fiber::http::HttpExchange &exchange,
                                                                      const fiber::http::HttpHeaders *headers) {
    co_return co_await exchange.send_header({
            .kind = fiber::http::OutgoingHeaderKind::Trailer,
            .headers = headers,
            .end_stream = true,
    });
}

std::string chain_to_string(fiber::mem::IoBufChain chain) {
    std::string out;
    while (auto *front = chain.front()) {
        if (front->readable() == 0) {
            chain.drop_empty_front();
            continue;
        }
        out.append(reinterpret_cast<const char *>(front->readable_data()), front->readable());
        chain.consume_and_compact(front->readable());
    }
    return out;
}

std::string recv_all(int fd) {
    std::string out;
    std::array<char, 4096> buf{};
    for (;;) {
        ssize_t rc = ::recv(fd, buf.data(), buf.size(), 0);
        if (rc == 0) {
            break;
        }
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        out.append(buf.data(), static_cast<size_t>(rc));
    }
    return out;
}

std::string recv_http_response(int fd) {
    std::string out;
    std::array<char, 4096> buf{};
    size_t header_end = std::string::npos;
    size_t content_length = 0;
    for (;;) {
        if (header_end != std::string::npos && out.size() >= header_end + content_length) {
            return out.substr(0, header_end + content_length);
        }
        ssize_t rc = ::recv(fd, buf.data(), buf.size(), 0);
        if (rc <= 0) {
            return out;
        }
        out.append(buf.data(), static_cast<size_t>(rc));
        if (header_end == std::string::npos) {
            size_t pos = out.find("\r\n\r\n");
            if (pos == std::string::npos) {
                continue;
            }
            header_end = pos + 4;
            size_t cl_pos = out.find("Content-Length:");
            if (cl_pos != std::string::npos) {
                cl_pos += sizeof("Content-Length:") - 1;
                while (cl_pos < pos && out[cl_pos] == ' ') {
                    ++cl_pos;
                }
                size_t cl_end = out.find("\r\n", cl_pos);
                if (cl_end != std::string::npos) {
                    content_length = static_cast<size_t>(std::stoul(out.substr(cl_pos, cl_end - cl_pos)));
                }
            }
        }
    }
}

struct HttpResponseReadResult {
    std::string response;
    std::string extra;
};

HttpResponseReadResult recv_http_response_with_extra(int fd) {
    std::string out;
    std::array<char, 4096> buf{};
    size_t header_end = std::string::npos;
    size_t content_length = 0;
    for (;;) {
        if (header_end != std::string::npos && out.size() >= header_end + content_length) {
            HttpResponseReadResult result;
            result.response = out.substr(0, header_end + content_length);
            result.extra = out.substr(header_end + content_length);
            return result;
        }
        ssize_t rc = ::recv(fd, buf.data(), buf.size(), 0);
        if (rc <= 0) {
            return {out, {}};
        }
        out.append(buf.data(), static_cast<size_t>(rc));
        if (header_end == std::string::npos) {
            size_t pos = out.find("\r\n\r\n");
            if (pos == std::string::npos) {
                continue;
            }
            header_end = pos + 4;
            size_t cl_pos = out.find("Content-Length:");
            if (cl_pos != std::string::npos) {
                cl_pos += sizeof("Content-Length:") - 1;
                while (cl_pos < pos && out[cl_pos] == ' ') {
                    ++cl_pos;
                }
                size_t cl_end = out.find("\r\n", cl_pos);
                if (cl_end != std::string::npos) {
                    content_length = static_cast<size_t>(std::stoul(out.substr(cl_pos, cl_end - cl_pos)));
                }
            }
        }
    }
}

int connect_client(uint16_t port) {
    int client = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (client < 0) {
        return -1;
    }
    timeval tv{};
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    fiber::net::SocketAddress target(fiber::net::IpAddress::loopback_v4(), port);
    sockaddr_storage storage{};
    socklen_t len = 0;
    if (!target.to_sockaddr(storage, len) || ::connect(client, reinterpret_cast<sockaddr *>(&storage), len) != 0) {
        ::close(client);
        return -1;
    }
    return client;
}

fiber::common::IoResult<uint16_t> resolve_port(int fd) {
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

DetachedTask start_server(fiber::event::EventLoop *loop, fiber::http::HttpHandler handler,
                          fiber::event::EventLoopGroup *worker_group, std::promise<uint16_t> *port_promise,
                          std::promise<fiber::http::Http1Server *> *server_promise,
                          fiber::http::HttpServerOptions http_options = {}) {
    fiber::net::ListenOptions options{};
    constexpr std::uint16_t kFirstTestPort = 20000;
    constexpr std::uint16_t kPortSpan = 20000;
    static std::atomic<std::uint32_t> next_test_port{kFirstTestPort};

    for (std::size_t i = 0; i < kPortSpan; ++i) {
        std::uint32_t next = next_test_port.fetch_add(1, std::memory_order_relaxed);
        std::uint16_t port = static_cast<std::uint16_t>(kFirstTestPort + ((next - kFirstTestPort) % kPortSpan));
        auto *server = new fiber::http::Http1Server(*loop, handler, http_options, worker_group);
        fiber::net::SocketAddress addr(fiber::net::IpAddress::loopback_v4(), port);
        auto bind_result = server->bind(addr, options);
        if (!bind_result) {
            delete server;
            continue;
        }

        port_promise->set_value(port);
        server_promise->set_value(server);
        fiber::async::spawn(*loop, [server]() { return server->serve(); });
        co_return;
    }

    port_promise->set_value(0);
    server_promise->set_value(nullptr);
    co_return;
}

DetachedTask stop_server(fiber::event::EventLoop *loop, fiber::http::Http1Server *server) {
    if (server) {
        co_await server->shutdown_and_wait();
    }
    loop->stop();
    co_return;
}

DetachedTask shutdown_server_and_signal(fiber::http::Http1Server *server, std::promise<void> *done) {
    co_await server->shutdown_and_wait();
    done->set_value();
    co_return;
}

} // namespace

TEST(Http1ServerTest, BasicGet) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<uint16_t> port_promise;
    std::promise<fiber::http::Http1Server *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() {
        auto handler = [](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
            fiber::http::HttpHeaders headers(exchange.pool());
            headers.set("Content-Type", "text/plain");
            auto header_result = co_await send_final_header(exchange, 200, &headers,
                                                            fiber::http::ResponseBodySpec::ContentLength(2), {}, false);
            if (!header_result) {
                co_return;
            }
            co_await exchange.write_all(reinterpret_cast<const uint8_t *>("ok"), 2, true);
            co_return;
        };
        return start_server(&group.at(0), handler, nullptr, &port_promise, &server_promise);
    });

    auto *server = server_future.get();
    ASSERT_NE(server, nullptr);
    uint16_t port = port_future.get();
    if (port == 0) {
        auto retry_port = resolve_port(server->fd());
        ASSERT_TRUE(retry_port.has_value());
        port = *retry_port;
    }
    ASSERT_NE(port, 0);

    int client = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    ASSERT_GE(client, 0);
    fiber::net::SocketAddress target(fiber::net::IpAddress::loopback_v4(), port);
    sockaddr_storage storage{};
    socklen_t len = 0;
    ASSERT_TRUE(target.to_sockaddr(storage, len));
    ASSERT_EQ(::connect(client, reinterpret_cast<sockaddr *>(&storage), len), 0);

    const char *request = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, request, std::strlen(request), 0), static_cast<ssize_t>(std::strlen(request)));

    std::string response = recv_all(client);
    ::close(client);

    EXPECT_NE(response.find("200"), std::string::npos);
    EXPECT_NE(response.find("ok"), std::string::npos);

    fiber::async::spawn(group.at(0), [&]() { return stop_server(&group.at(0), server); });
    group.join();
    delete server;
}

TEST(Http1ServerTest, CachesImportantRequestHeaderPointers) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<uint16_t> port_promise;
    std::promise<fiber::http::Http1Server *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() {
        auto handler = [](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
            const auto *host = exchange.host_header();
            const auto *content_type = exchange.content_type_header();
            const auto *range = exchange.range_header();
            const auto *if_range = exchange.if_range_header();
            const auto *expect = exchange.expect_header();
            const bool ok = host && host->value_view() == "localhost" && content_type &&
                            content_type->value_view() == "text/plain" && range && range->value_view() == "bytes=0-9" &&
                            if_range && if_range->value_view() == "\"etag\"" && expect &&
                            expect->value_view() == "100-continue";
            auto header_result =
                    co_await send_final_header(exchange, ok ? 200 : 500, nullptr,
                                               fiber::http::ResponseBodySpec::ContentLength(ok ? 2 : 3), {}, false);
            if (!header_result) {
                co_return;
            }
            co_await exchange.write_all(reinterpret_cast<const uint8_t *>(ok ? "ok" : "bad"), ok ? 2 : 3, true);
            co_return;
        };
        return start_server(&group.at(0), handler, nullptr, &port_promise, &server_promise);
    });

    auto *server = server_future.get();
    ASSERT_NE(server, nullptr);
    uint16_t port = port_future.get();
    if (port == 0) {
        auto retry_port = resolve_port(server->fd());
        ASSERT_TRUE(retry_port.has_value());
        port = *retry_port;
    }
    ASSERT_NE(port, 0);

    int client = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    ASSERT_GE(client, 0);
    fiber::net::SocketAddress target(fiber::net::IpAddress::loopback_v4(), port);
    sockaddr_storage storage{};
    socklen_t len = 0;
    ASSERT_TRUE(target.to_sockaddr(storage, len));
    ASSERT_EQ(::connect(client, reinterpret_cast<sockaddr *>(&storage), len), 0);

    const char *request = "GET / HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Content-Type: text/plain\r\n"
                          "Range: bytes=0-9\r\n"
                          "If-Range: \"etag\"\r\n"
                          "Expect: 100-continue\r\n"
                          "Connection: close\r\n"
                          "\r\n";
    ASSERT_EQ(::send(client, request, std::strlen(request), 0), static_cast<ssize_t>(std::strlen(request)));

    std::string response = recv_all(client);
    ::close(client);

    EXPECT_NE(response.find("200"), std::string::npos);
    EXPECT_NE(response.find("ok"), std::string::npos);

    fiber::async::spawn(group.at(0), [&]() { return stop_server(&group.at(0), server); });
    group.join();
    delete server;
}

TEST(Http1ServerTest, CanSendContinueHeaderBeforeFinalResponse) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<uint16_t> port_promise;
    std::promise<fiber::http::Http1Server *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() {
        auto handler = [](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
            auto continue_result = co_await exchange.send_continue_header();
            if (!continue_result) {
                co_return;
            }
            co_await send_final_header(exchange, 204, nullptr, fiber::http::ResponseBodySpec::None(), {}, true);
            co_return;
        };
        return start_server(&group.at(0), handler, nullptr, &port_promise, &server_promise);
    });

    auto *server = server_future.get();
    ASSERT_NE(server, nullptr);
    uint16_t port = port_future.get();
    if (port == 0) {
        auto retry_port = resolve_port(server->fd());
        ASSERT_TRUE(retry_port.has_value());
        port = *retry_port;
    }
    ASSERT_NE(port, 0);

    int client = connect_client(port);
    ASSERT_GE(client, 0);

    const char *request = "POST /continue HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Expect: 100-continue\r\n"
                          "Connection: close\r\n"
                          "Content-Length: 0\r\n"
                          "\r\n";
    ASSERT_EQ(::send(client, request, std::strlen(request), 0), static_cast<ssize_t>(std::strlen(request)));

    std::string response = recv_all(client);
    ::close(client);

    const auto first_pos = response.find("HTTP/1.1 100 Continue\r\n\r\n");
    const auto final_pos = response.find("HTTP/1.1 204 No Content\r\n");
    EXPECT_NE(first_pos, std::string::npos);
    EXPECT_NE(final_pos, std::string::npos);
    EXPECT_LT(first_pos, final_pos);
    EXPECT_EQ(response.find("Content-Length:"), std::string::npos);
    EXPECT_EQ(response.find("Transfer-Encoding:"), std::string::npos);

    fiber::async::spawn(group.at(0), [&]() { return stop_server(&group.at(0), server); });
    group.join();
    delete server;
}

TEST(Http1ServerTest, CloseResponseSkipsConfiguredUnreadBodyDrain) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<uint16_t> port_promise;
    std::promise<fiber::http::Http1Server *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() {
        auto handler = [](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
            (void) co_await send_final_header(exchange, 413, nullptr, fiber::http::ResponseBodySpec::ContentLength(0),
                                              fiber::http::ResponseConnectionMode::Close, true);
        };
        fiber::http::HttpServerOptions options;
        options.drain_unread_body = true;
        return start_server(&group.at(0), handler, nullptr, &port_promise, &server_promise, options);
    });

    auto *server = server_future.get();
    ASSERT_NE(server, nullptr);
    const uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    const int client = connect_client(port);
    ASSERT_GE(client, 0);
    const char request[] = "POST /upload HTTP/1.1\r\n"
                           "Host: localhost\r\n"
                           "Content-Length: 5\r\n"
                           "\r\n";
    ASSERT_EQ(::send(client, request, sizeof(request) - 1, 0), static_cast<ssize_t>(sizeof(request) - 1));

    const std::string response = recv_http_response(client);
    EXPECT_NE(response.find("HTTP/1.1 413 Payload Too Large\r\n"), std::string::npos) << response;
    EXPECT_NE(response.find("Connection: close\r\n"), std::string::npos) << response;

    pollfd descriptor{
            .fd = client,
            .events = POLLIN,
    };
    ASSERT_GT(::poll(&descriptor, 1, 500), 0);
    char byte = 0;
    EXPECT_EQ(::recv(client, &byte, 1, 0), 0);
    ::close(client);

    fiber::async::spawn(group.at(0), [&]() { return stop_server(&group.at(0), server); });
    group.join();
    delete server;
}

TEST(Http1ServerTest, WriteBodyWithoutExplicitHeaderAutoUsesChunkedForStreaming) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<uint16_t> port_promise;
    std::promise<fiber::http::Http1Server *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() {
        auto handler = [](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
            auto header_result = co_await send_final_header(exchange, 200, nullptr,
                                                            fiber::http::ResponseBodySpec::Auto(), {}, false);
            if (!header_result) {
                co_return;
            }
            auto first = co_await exchange.write_all(reinterpret_cast<const uint8_t *>("he"), 2, false);
            if (!first) {
                co_return;
            }
            co_await exchange.write_all(reinterpret_cast<const uint8_t *>("llo"), 3, true);
            co_return;
        };
        return start_server(&group.at(0), handler, nullptr, &port_promise, &server_promise);
    });

    uint16_t port = port_future.get();
    ASSERT_NE(port, 0);
    auto *server = server_future.get();
    ASSERT_NE(server, nullptr);

    int client = connect_client(port);
    ASSERT_GE(client, 0);

    const char *request = "GET /stream HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, request, std::strlen(request), 0), static_cast<ssize_t>(std::strlen(request)));

    std::string response = recv_all(client);
    ::close(client);

    EXPECT_NE(response.find("Transfer-Encoding: chunked"), std::string::npos) << response;
    EXPECT_NE(response.find("\r\n2\r\nhe\r\n3\r\nllo\r\n0\r\n\r\n"), std::string::npos) << response;

    fiber::async::spawn(group.at(0), [&]() { return stop_server(&group.at(0), server); });
    group.join();
    delete server;
}

TEST(Http1ServerTest, WriteBodyWithoutExplicitHeaderAutoUsesContentLengthForLargeSingleBuffer) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<uint16_t> port_promise;
    std::promise<fiber::http::Http1Server *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() {
        auto handler = [](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
            std::string body(256, 'x');
            auto header_result = co_await send_final_header(
                    exchange, 200, nullptr, fiber::http::ResponseBodySpec::ContentLength(body.size()), {}, false);
            if (!header_result) {
                co_return;
            }
            auto write_result =
                    co_await exchange.write_all(reinterpret_cast<const uint8_t *>(body.data()), body.size(), true);
            if (!write_result) {
                co_return;
            }
            co_return;
        };
        return start_server(&group.at(0), handler, nullptr, &port_promise, &server_promise);
    });

    uint16_t port = port_future.get();
    ASSERT_NE(port, 0);
    auto *server = server_future.get();
    ASSERT_NE(server, nullptr);

    int client = connect_client(port);
    ASSERT_GE(client, 0);

    const char *request = "GET /large HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, request, std::strlen(request), 0), static_cast<ssize_t>(std::strlen(request)));

    std::string response = recv_all(client);
    ::close(client);

    EXPECT_NE(response.find("Content-Length: 256"), std::string::npos) << response;
    EXPECT_EQ(response.find("Transfer-Encoding: chunked"), std::string::npos) << response;
    EXPECT_NE(response.find("\r\n\r\n" + std::string(256, 'x')), std::string::npos) << response;

    fiber::async::spawn(group.at(0), [&]() { return stop_server(&group.at(0), server); });
    group.join();
    delete server;
}

TEST(Http1ServerTest, StreamResponseSwitchesToRawBidirectionalIo) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<uint16_t> port_promise;
    std::promise<fiber::http::Http1Server *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() {
        auto handler = [](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
            if (!exchange.request_body_spec().is_none()) {
                co_return;
            }

            fiber::http::HttpHeaders headers(exchange.pool());
            headers.set("Upgrade", "websocket");
            headers.set("Connection", "Upgrade");
            auto header_result = co_await send_final_header(exchange, 101, &headers,
                                                            fiber::http::ResponseBodySpec::Stream(), {}, false);
            if (!header_result) {
                co_return;
            }

            auto read_result = co_await exchange.read_body(64);
            if (!read_result || read_result->complete()) {
                co_return;
            }
            std::string response = "server:";
            response.append(chain_to_string(std::move(*read_result)));
            auto eof_result = co_await exchange.read_body(64);
            if (!eof_result || !eof_result->complete()) {
                co_return;
            }
            response.append(chain_to_string(std::move(*eof_result)));
            (void) co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>(response.data()), response.size(),
                                               true);
            co_return;
        };
        return start_server(&group.at(0), handler, nullptr, &port_promise, &server_promise);
    });

    uint16_t port = port_future.get();
    ASSERT_NE(port, 0);
    auto *server = server_future.get();
    ASSERT_NE(server, nullptr);

    int client = connect_client(port);
    ASSERT_GE(client, 0);
    const char *request = "GET /chat HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Upgrade: websocket\r\n"
                          "Connection: Upgrade\r\n"
                          "\r\n"
                          "client-data";
    ASSERT_EQ(::send(client, request, std::strlen(request), 0), static_cast<ssize_t>(std::strlen(request)));
    ASSERT_EQ(::shutdown(client, SHUT_WR), 0);

    std::string response = recv_all(client);
    ::close(client);

    EXPECT_NE(response.find("HTTP/1.1 101 Switching Protocols\r\n"), std::string::npos) << response;
    EXPECT_NE(response.find("Upgrade: websocket\r\n"), std::string::npos) << response;
    EXPECT_EQ(response.find("Content-Length:"), std::string::npos) << response;
    EXPECT_EQ(response.find("Transfer-Encoding:"), std::string::npos) << response;
    EXPECT_EQ(response.find("Connection: close"), std::string::npos) << response;
    EXPECT_NE(response.find("\r\n\r\nserver:client-data"), std::string::npos) << response;

    fiber::async::spawn(group.at(0), [&]() { return stop_server(&group.at(0), server); });
    group.join();
    delete server;
}

TEST(Http1ServerTest, ChunkedPost) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<uint16_t> port_promise;
    std::promise<fiber::http::Http1Server *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() {
        auto handler = [](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
            if (!exchange.request_body_spec().is_chunked()) {
                co_await send_final_header(exchange, 500, nullptr, fiber::http::ResponseBodySpec::ContentLength(0),
                                           fiber::http::ResponseConnectionMode::Close, true);
                co_return;
            }
            std::string body;
            for (;;) {
                auto read_result = co_await exchange.read_body(64);
                if (!read_result) {
                    co_await send_final_header(exchange, 400, nullptr, fiber::http::ResponseBodySpec::ContentLength(0),
                                               fiber::http::ResponseConnectionMode::Close, true);
                    co_return;
                }
                const bool last = read_result->complete();
                if (read_result->readable_bytes() > 0) {
                    body.append(chain_to_string(std::move(*read_result)));
                }
                if (last) {
                    break;
                }
            }
            fiber::http::HttpHeaders headers(exchange.pool());
            headers.set("Content-Type", "text/plain");
            auto header_result = co_await send_final_header(
                    exchange, 200, &headers, fiber::http::ResponseBodySpec::ContentLength(body.size()), {}, false);
            if (!header_result) {
                co_return;
            }
            if (!body.empty()) {
                co_await exchange.write_all(reinterpret_cast<const uint8_t *>(body.data()), body.size(), true);
            } else {
                co_await exchange.write_all(nullptr, 0, true);
            }
            co_return;
        };
        return start_server(&group.at(0), handler, nullptr, &port_promise, &server_promise);
    });

    uint16_t port = port_future.get();
    ASSERT_NE(port, 0);
    auto *server = server_future.get();
    ASSERT_NE(server, nullptr);

    int client = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    ASSERT_GE(client, 0);
    fiber::net::SocketAddress target(fiber::net::IpAddress::loopback_v4(), port);
    sockaddr_storage storage{};
    socklen_t len = 0;
    ASSERT_TRUE(target.to_sockaddr(storage, len));
    ASSERT_EQ(::connect(client, reinterpret_cast<sockaddr *>(&storage), len), 0);

    const char *request = "POST /echo HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Transfer-Encoding: chunked\r\n"
                          "Connection: close\r\n"
                          "\r\n"
                          "4\r\nWiki\r\n"
                          "5\r\npedia\r\n"
                          "0\r\n\r\n";
    ASSERT_EQ(::send(client, request, std::strlen(request), 0), static_cast<ssize_t>(std::strlen(request)));

    std::string response = recv_all(client);
    ::close(client);

    EXPECT_NE(response.find("200"), std::string::npos);
    EXPECT_NE(response.find("Wikipedia"), std::string::npos);

    fiber::async::spawn(group.at(0), [&]() { return stop_server(&group.at(0), server); });
    group.join();
    delete server;
}

TEST(Http1ServerTest, WriteBodyAcceptsIoBufChain) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<uint16_t> port_promise;
    std::promise<fiber::http::Http1Server *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() {
        auto handler = [](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
            const auto request_body = exchange.request_body_spec();
            if (!request_body.is_content_length() || request_body.content_length() != 9) {
                co_await send_final_header(exchange, 500, nullptr, fiber::http::ResponseBodySpec::ContentLength(0),
                                           fiber::http::ResponseConnectionMode::Close, true);
                co_return;
            }
            std::vector<fiber::mem::IoBufChain> chunks;

            for (;;) {
                auto read_result = co_await exchange.read_body(4);
                if (!read_result) {
                    co_return;
                }

                bool last = read_result->complete();
                chunks.push_back(std::move(*read_result));
                if (last) {
                    break;
                }
            }

            auto header_result = co_await send_final_header(exchange, 200, nullptr,
                                                            fiber::http::ResponseBodySpec::ContentLength(9), {}, false);
            if (!header_result) {
                co_return;
            }

            for (auto &chunk: chunks) {
                auto write_result = co_await exchange.write_all(std::move(chunk));
                if (!write_result) {
                    co_return;
                }
            }
            co_return;
        };
        return start_server(&group.at(0), handler, nullptr, &port_promise, &server_promise);
    });

    uint16_t port = port_future.get();
    ASSERT_NE(port, 0);
    auto *server = server_future.get();
    ASSERT_NE(server, nullptr);

    int client = connect_client(port);
    ASSERT_GE(client, 0);

    const char *request = "POST /echo HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Content-Length: 9\r\n"
                          "Connection: close\r\n"
                          "\r\n"
                          "Wikipedia";
    ASSERT_EQ(::send(client, request, std::strlen(request), 0), static_cast<ssize_t>(std::strlen(request)));

    std::string response = recv_all(client);
    ::close(client);

    EXPECT_NE(response.find("200"), std::string::npos) << response;
    EXPECT_NE(response.find("Content-Length: 9"), std::string::npos) << response;
    EXPECT_NE(response.find("\r\n\r\nWikipedia"), std::string::npos) << response;

    fiber::async::spawn(group.at(0), [&]() { return stop_server(&group.at(0), server); });
    group.join();
    delete server;
}

TEST(Http1ServerTest, ChunkedPostTrailersAreAvailableAfterBody) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<uint16_t> port_promise;
    std::promise<fiber::http::Http1Server *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() {
        auto handler = [](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
            std::string body;
            for (;;) {
                auto read_result = co_await exchange.read_body(64);
                if (!read_result) {
                    co_await send_final_header(exchange, 400, nullptr, fiber::http::ResponseBodySpec::ContentLength(0),
                                               fiber::http::ResponseConnectionMode::Close, true);
                    co_return;
                }
                const bool last = read_result->complete();
                if (read_result->readable_bytes() > 0) {
                    body.append(chain_to_string(std::move(*read_result)));
                }
                if (last) {
                    break;
                }
            }

            std::string response = body;
            response.push_back('|');
            response.append(exchange.request_trailers().get("x-very-long-trailer-name-that-exceeds-parser-cache"));

            auto header_result = co_await send_final_header(
                    exchange, 200, nullptr, fiber::http::ResponseBodySpec::ContentLength(response.size()), {}, false);
            if (!header_result) {
                co_return;
            }
            co_await exchange.write_all(reinterpret_cast<const uint8_t *>(response.data()), response.size(), true);
            co_return;
        };
        return start_server(&group.at(0), handler, nullptr, &port_promise, &server_promise);
    });

    uint16_t port = port_future.get();
    ASSERT_NE(port, 0);
    auto *server = server_future.get();
    ASSERT_NE(server, nullptr);

    int client = connect_client(port);
    ASSERT_GE(client, 0);

    const char *request = "POST /trailers HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Transfer-Encoding: chunked\r\n"
                          "Connection: close\r\n"
                          "\r\n"
                          "4\r\nWiki\r\n"
                          "5\r\npedia\r\n"
                          "0\r\n"
                          "X-Very-Long-Trailer-Name-That-Exceeds-Parser-Cache: sha-256=xyz\r\n"
                          "\r\n";
    ASSERT_EQ(::send(client, request, std::strlen(request), 0), static_cast<ssize_t>(std::strlen(request)));

    std::string response = recv_all(client);
    ::close(client);

    EXPECT_NE(response.find("200"), std::string::npos);
    EXPECT_NE(response.find("Wikipedia|sha-256=xyz"), std::string::npos);

    fiber::async::spawn(group.at(0), [&]() { return stop_server(&group.at(0), server); });
    group.join();
    delete server;
}

TEST(Http1ServerTest, ChunkedPostWaitsForCompleteTrailersBeforeLastChunk) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<uint16_t> port_promise;
    std::promise<fiber::http::Http1Server *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() {
        auto handler = [](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
            std::string body;
            for (;;) {
                auto read_result = co_await exchange.read_body(64);
                if (!read_result) {
                    co_await send_final_header(exchange, 400, nullptr, fiber::http::ResponseBodySpec::ContentLength(0),
                                               fiber::http::ResponseConnectionMode::Close, true);
                    co_return;
                }
                const bool last = read_result->complete();
                if (read_result->readable_bytes() > 0) {
                    body.append(chain_to_string(std::move(*read_result)));
                }
                if (last) {
                    break;
                }
            }

            std::string response = body;
            response.push_back('|');
            response.append(exchange.request_trailers().get("digest"));

            auto header_result = co_await send_final_header(
                    exchange, 200, nullptr, fiber::http::ResponseBodySpec::ContentLength(response.size()), {}, false);
            if (!header_result) {
                co_return;
            }
            co_await exchange.write_all(reinterpret_cast<const uint8_t *>(response.data()), response.size(), true);
            co_return;
        };
        return start_server(&group.at(0), handler, nullptr, &port_promise, &server_promise);
    });

    uint16_t port = port_future.get();
    ASSERT_NE(port, 0);
    auto *server = server_future.get();
    ASSERT_NE(server, nullptr);

    int client = connect_client(port);
    ASSERT_GE(client, 0);

    const char *part1 = "POST /trailers HTTP/1.1\r\n"
                        "Host: localhost\r\n"
                        "Transfer-Encoding: chunked\r\n"
                        "Connection: close\r\n"
                        "\r\n"
                        "4\r\nWiki\r\n"
                        "5\r\npedia\r\n"
                        "0\r\n"
                        "Digest: sha-25";
    ASSERT_EQ(::send(client, part1, std::strlen(part1), 0), static_cast<ssize_t>(std::strlen(part1)));

    std::thread sender([client]() {
        std::this_thread::sleep_for(50ms);
        const char *part2 = "6=xyz\r\n\r\n";
        EXPECT_EQ(::send(client, part2, std::strlen(part2), 0), static_cast<ssize_t>(std::strlen(part2)));
    });

    std::string response = recv_all(client);
    sender.join();
    ::close(client);

    EXPECT_NE(response.find("200"), std::string::npos);
    EXPECT_NE(response.find("Wikipedia|sha-256=xyz"), std::string::npos);

    fiber::async::spawn(group.at(0), [&]() { return stop_server(&group.at(0), server); });
    group.join();
    delete server;
}

TEST(Http1ServerTest, InvalidChunkedPostReturnsBadRequest) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<uint16_t> port_promise;
    std::promise<fiber::http::Http1Server *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() {
        auto handler = [](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
            for (;;) {
                auto read_result = co_await exchange.read_body(64);
                if (!read_result) {
                    co_await send_final_header(exchange, 400, nullptr, fiber::http::ResponseBodySpec::ContentLength(0),
                                               fiber::http::ResponseConnectionMode::Close, true);
                    co_return;
                }
                if (read_result->complete()) {
                    break;
                }
            }
            co_await send_final_header(exchange, 204, nullptr, fiber::http::ResponseBodySpec::None(), {}, true);
            co_await exchange.write_all(nullptr, 0, true);
            co_return;
        };
        return start_server(&group.at(0), handler, nullptr, &port_promise, &server_promise);
    });

    uint16_t port = port_future.get();
    ASSERT_NE(port, 0);
    auto *server = server_future.get();
    ASSERT_NE(server, nullptr);

    int client = connect_client(port);
    ASSERT_GE(client, 0);

    const char *request = "POST /echo HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Transfer-Encoding: chunked\r\n"
                          "Connection: close\r\n"
                          "\r\n"
                          "+4\r\nWiki\r\n"
                          "0\r\n\r\n";
    ASSERT_EQ(::send(client, request, std::strlen(request), 0), static_cast<ssize_t>(std::strlen(request)));

    std::string response = recv_all(client);
    ::close(client);

    EXPECT_NE(response.find("400 Bad Request"), std::string::npos);

    fiber::async::spawn(group.at(0), [&]() { return stop_server(&group.at(0), server); });
    group.join();
    delete server;
}

TEST(Http1ServerTest, ChunkedResponseCanSendTrailers) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<uint16_t> port_promise;
    std::promise<fiber::http::Http1Server *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() {
        auto handler = [](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
            fiber::http::HttpHeaders headers(exchange.pool());
            headers.set("Trailer", "digest, x-md5");
            auto header_result = co_await send_final_header(exchange, 200, &headers,
                                                            fiber::http::ResponseBodySpec::Chunked(), {}, false);
            if (!header_result) {
                co_return;
            }

            auto body_result = co_await exchange.write_all(reinterpret_cast<const uint8_t *>("hello"), 5, false);
            if (!body_result) {
                co_return;
            }

            fiber::http::HttpHeaders trailers(exchange.pool());
            trailers.set("digest", "sha-256=xyz");
            trailers.set("x-md5", "abc123");
            co_await send_trailer_header(exchange, &trailers);
            co_return;
        };
        return start_server(&group.at(0), handler, nullptr, &port_promise, &server_promise);
    });

    uint16_t port = port_future.get();
    ASSERT_NE(port, 0);
    auto *server = server_future.get();
    ASSERT_NE(server, nullptr);

    int client = connect_client(port);
    ASSERT_GE(client, 0);

    const char *request = "GET /trailers HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Connection: close\r\n"
                          "\r\n";
    ASSERT_EQ(::send(client, request, std::strlen(request), 0), static_cast<ssize_t>(std::strlen(request)));

    std::string response = recv_all(client);
    ::close(client);

    EXPECT_NE(response.find("Transfer-Encoding: chunked"), std::string::npos);
    EXPECT_NE(response.find("Trailer: digest, x-md5"), std::string::npos);
    EXPECT_NE(response.find("\r\n5\r\nhello\r\n0\r\n"), std::string::npos);
    EXPECT_NE(response.find("digest: sha-256=xyz\r\n"), std::string::npos);
    EXPECT_NE(response.find("x-md5: abc123\r\n"), std::string::npos);

    fiber::async::spawn(group.at(0), [&]() { return stop_server(&group.at(0), server); });
    group.join();
    delete server;
}

TEST(Http1ServerTest, KeepAliveReuse) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<uint16_t> port_promise;
    std::promise<fiber::http::Http1Server *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();
    std::atomic<int> request_count{0};

    fiber::async::spawn(group.at(0), [&]() {
        auto handler = [&](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
            int count = request_count.fetch_add(1, std::memory_order_relaxed) + 1;
            std::string body = std::to_string(count);
            auto header_result = co_await send_final_header(
                    exchange, 200, nullptr, fiber::http::ResponseBodySpec::ContentLength(body.size()), {}, false);
            if (!header_result) {
                co_return;
            }
            co_await exchange.write_all(reinterpret_cast<const uint8_t *>(body.data()), body.size(), true);
            co_return;
        };
        return start_server(&group.at(0), handler, nullptr, &port_promise, &server_promise);
    });

    uint16_t port = port_future.get();
    ASSERT_NE(port, 0);
    auto *server = server_future.get();
    ASSERT_NE(server, nullptr);

    int client = connect_client(port);
    ASSERT_GE(client, 0);

    const char *request1 = "GET /one HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ASSERT_EQ(::send(client, request1, std::strlen(request1), 0), static_cast<ssize_t>(std::strlen(request1)));
    std::string response1 = recv_http_response(client);
    EXPECT_NE(response1.find("\r\n\r\n1"), std::string::npos);

    const char *request2 = "GET /two HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, request2, std::strlen(request2), 0), static_cast<ssize_t>(std::strlen(request2)));
    std::string response2 = recv_all(client);
    ::close(client);

    EXPECT_NE(response2.find("\r\n\r\n2"), std::string::npos);
    EXPECT_EQ(request_count.load(std::memory_order_relaxed), 2);

    fiber::async::spawn(group.at(0), [&]() { return stop_server(&group.at(0), server); });
    group.join();
    delete server;
}

TEST(Http1ServerTest, ChunkedKeepAlivePipelinedNextRequest) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<uint16_t> port_promise;
    std::promise<fiber::http::Http1Server *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();
    std::atomic<int> request_count{0};

    fiber::async::spawn(group.at(0), [&]() {
        auto handler = [&](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
            int count = request_count.fetch_add(1, std::memory_order_relaxed) + 1;
            std::string body;
            for (;;) {
                auto read_result = co_await exchange.read_body(64);
                if (!read_result) {
                    co_await send_final_header(exchange, 400, nullptr, fiber::http::ResponseBodySpec::ContentLength(0),
                                               fiber::http::ResponseConnectionMode::Close, true);
                    co_return;
                }
                const bool last = read_result->complete();
                if (read_result->readable_bytes() > 0) {
                    body.append(chain_to_string(std::move(*read_result)));
                }
                if (last) {
                    break;
                }
            }

            std::string response = std::to_string(count);
            response.push_back(':');
            response.append(body);
            if (count == 1) {
                response.push_back('|');
                response.append(exchange.request_trailers().get("digest"));
            }

            auto header_result = co_await send_final_header(
                    exchange, 200, nullptr, fiber::http::ResponseBodySpec::ContentLength(response.size()),
                    count == 1 ? fiber::http::ResponseConnectionMode::Auto : fiber::http::ResponseConnectionMode::Close,
                    false);
            if (!header_result) {
                co_return;
            }
            co_await exchange.write_all(reinterpret_cast<const uint8_t *>(response.data()), response.size(), true);
            co_return;
        };
        return start_server(&group.at(0), handler, nullptr, &port_promise, &server_promise);
    });

    uint16_t port = port_future.get();
    ASSERT_NE(port, 0);
    auto *server = server_future.get();
    ASSERT_NE(server, nullptr);

    int client = connect_client(port);
    ASSERT_GE(client, 0);

    const char *request = "POST /one HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Transfer-Encoding: chunked\r\n"
                          "\r\n"
                          "4\r\nWiki\r\n"
                          "5\r\npedia\r\n"
                          "0\r\n"
                          "Digest: sha-256=xyz\r\n"
                          "\r\n"
                          "GET /two HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Connection: close\r\n"
                          "\r\n";
    ASSERT_EQ(::send(client, request, std::strlen(request), 0), static_cast<ssize_t>(std::strlen(request)));

    HttpResponseReadResult first_read = recv_http_response_with_extra(client);
    std::string response1 = std::move(first_read.response);
    std::string response2 = std::move(first_read.extra);
    response2.append(recv_all(client));
    ::close(client);

    EXPECT_NE(response1.find("\r\n\r\n1:Wikipedia|sha-256=xyz"), std::string::npos);
    EXPECT_NE(response2.find("\r\n\r\n2:"), std::string::npos);
    EXPECT_EQ(request_count.load(std::memory_order_relaxed), 2);

    fiber::async::spawn(group.at(0), [&]() { return stop_server(&group.at(0), server); });
    group.join();
    delete server;
}

TEST(Http1ServerTest, EventLoopGroupDispatch) {
    fiber::event::EventLoopGroup group(2);
    group.start();

    std::promise<uint16_t> port_promise;
    std::promise<fiber::http::Http1Server *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();
    std::atomic<bool> saw_worker_loop{false};

    fiber::async::spawn(group.at(0), [&]() -> fiber::async::DetachedTask {
        auto handler = [&](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
            if (&fiber::event::EventLoop::current() == &group.at(1)) {
                saw_worker_loop.store(true, std::memory_order_release);
            }
            auto header_result = co_await send_final_header(exchange, 200, nullptr,
                                                            fiber::http::ResponseBodySpec::ContentLength(2), {}, false);
            if (!header_result) {
                co_return;
            }
            co_await exchange.write_all(reinterpret_cast<const uint8_t *>("ok"), 2, true);
            co_return;
        };
        auto *server = new fiber::http::Http1Server(group.at(0), handler, {}, &group);
        fiber::net::ListenOptions options{};
        fiber::net::SocketAddress addr(fiber::net::IpAddress::loopback_v4(), 0);
        auto bind_result = server->bind(addr, options);
        if (!bind_result) {
            port_promise.set_value(0);
            server_promise.set_value(nullptr);
            delete server;
            co_return;
        }
        auto port = resolve_port(server->fd());
        port_promise.set_value(port ? *port : 0);
        server_promise.set_value(server);
        fiber::async::spawn(group.at(0), [server]() { return server->serve(); });
        co_return;
    });

    uint16_t port = port_future.get();
    ASSERT_NE(port, 0);
    auto *server = server_future.get();
    ASSERT_NE(server, nullptr);

    for (int i = 0; i < 2; ++i) {
        int client = connect_client(port);
        ASSERT_GE(client, 0);
        const char *request = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        ASSERT_EQ(::send(client, request, std::strlen(request), 0), static_cast<ssize_t>(std::strlen(request)));
        std::string response = recv_all(client);
        ::close(client);
        EXPECT_NE(response.find("200"), std::string::npos);
    }

    EXPECT_TRUE(saw_worker_loop.load(std::memory_order_acquire));

    std::promise<void> shutdown_promise;
    auto shutdown_future = shutdown_promise.get_future();
    fiber::async::spawn(group.at(0), [server, &shutdown_promise]() {
        return shutdown_server_and_signal(server, &shutdown_promise);
    });
    ASSERT_EQ(shutdown_future.wait_for(2s), std::future_status::ready);
    group.stop();
    group.join();
    delete server;
}

TEST(Http1ServerTest, ShutdownAndWait) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<uint16_t> port_promise;
    std::promise<fiber::http::Http1Server *> server_promise;
    std::promise<void> entered_promise;
    std::promise<void> shutdown_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();
    auto entered_future = entered_promise.get_future();
    auto shutdown_future = shutdown_promise.get_future();
    std::atomic<bool> release_handler{false};

    fiber::async::spawn(group.at(0), [&]() {
        auto handler = [&](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
            entered_promise.set_value();
            while (!release_handler.load(std::memory_order_acquire)) {
                co_await fiber::async::sleep(10ms);
            }
            auto header_result = co_await send_final_header(exchange, 200, nullptr,
                                                            fiber::http::ResponseBodySpec::ContentLength(2), {}, false);
            if (!header_result) {
                co_return;
            }
            co_await exchange.write_all(reinterpret_cast<const uint8_t *>("ok"), 2, true);
            co_return;
        };
        return start_server(&group.at(0), handler, nullptr, &port_promise, &server_promise);
    });

    uint16_t port = port_future.get();
    ASSERT_NE(port, 0);
    auto *server = server_future.get();
    ASSERT_NE(server, nullptr);

    int client = connect_client(port);
    ASSERT_GE(client, 0);
    const char *request = "GET /slow HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ASSERT_EQ(::send(client, request, std::strlen(request), 0), static_cast<ssize_t>(std::strlen(request)));

    entered_future.get();
    fiber::async::spawn(group.at(0), [server, &shutdown_promise]() {
        return shutdown_server_and_signal(server, &shutdown_promise);
    });

    EXPECT_EQ(shutdown_future.wait_for(100ms), std::future_status::timeout);

    release_handler.store(true, std::memory_order_release);
    std::string response = recv_all(client);
    ::close(client);

    EXPECT_NE(response.find("Connection: close"), std::string::npos);
    EXPECT_EQ(shutdown_future.wait_for(2s), std::future_status::ready);

    group.stop();
    group.join();
    delete server;
}
