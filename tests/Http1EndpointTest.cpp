#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/async/Task.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/HttpExchange.h>
#include <fiber/http/HttpHeaders.h>
#include <fiber/http/Server.h>
#include <fiber/http/endpoint/Http1Endpoint.h>
#include <fiber/net/IpAddress.h>
#include <fiber/net/SocketAddress.h>

namespace {

using fiber::async::DetachedTask;
using fiber::http::Http1Endpoint;
using fiber::http::Server;
using namespace std::chrono_literals;

// Blocking client socket helpers: the tests drive the server with raw syscalls
// so nothing on the client side depends on the code under test.
int connect_to(std::uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    fiber::net::SocketAddress target(fiber::net::IpAddress::loopback_v4(), port);
    sockaddr_storage storage{};
    socklen_t len = 0;
    if (!target.to_sockaddr(storage, len) || ::connect(fd, reinterpret_cast<sockaddr *>(&storage), len) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool send_all(int fd, std::string_view data) {
    return ::send(fd, data.data(), data.size(), 0) == static_cast<ssize_t>(data.size());
}

void set_recv_timeout(int fd, std::chrono::milliseconds timeout) {
    timeval tv{};
    tv.tv_sec = static_cast<time_t>(timeout.count() / 1000);
    tv.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

// Reads until EOF (or the receive timeout fires).
std::string recv_all(int fd) {
    std::string out;
    char buffer[4096];
    for (;;) {
        ssize_t received = ::recv(fd, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            break;
        }
        out.append(buffer, static_cast<std::size_t>(received));
    }
    return out;
}

// Reads until the response body terminator shows up, so a test can look at a
// response without waiting for the peer to close.
std::string recv_until(int fd, std::string_view needle) {
    std::string out;
    char buffer[4096];
    while (out.find(needle) == std::string::npos) {
        ssize_t received = ::recv(fd, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            break;
        }
        out.append(buffer, static_cast<std::size_t>(received));
    }
    return out;
}

fiber::async::Task<void> write_text(fiber::http::HttpExchange &exchange, int status, std::string_view body) {
    fiber::http::HttpHeaders headers(exchange.pool());
    headers.set("Content-Type", "text/plain");
    auto sent = co_await exchange.send_header({
            .kind = fiber::http::OutgoingHeaderKind::Final,
            .status_code = status,
            .headers = &headers,
            .body = fiber::http::ResponseBodySpec::ContentLength(body.size()),
            .end_stream = body.empty(),
    });
    if (!sent || body.empty()) {
        co_return;
    }
    (void) co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>(body.data()), body.size(), true);
    co_return;
}

// A server plus the endpoint it owns, wired onto loop 0 of `group` and already
// serving. `port` is the kernel-assigned port, so tests never pick one.
struct RunningServer {
    std::unique_ptr<Server> server{};
    Http1Endpoint *endpoint = nullptr;
    std::uint16_t port = 0;
    std::future<void> serve_done{};

    RunningServer() = default;
    // A user-declared destructor suppresses the implicit move, and start_server
    // returns by value.
    RunningServer(RunningServer &&) = default;
    RunningServer &operator=(RunningServer &&) = default;

    void stop_and_join() {
        server->stop();
        serve_done.get();
    }

    ~RunningServer() {
        if (server && server->state() != Server::State::Stopped && server->state() != Server::State::Created) {
            stop_and_join();
        }
    }
};

RunningServer start_server(fiber::event::EventLoopGroup &group, Http1Endpoint::Options options,
                           fiber::event::EventLoopGroup *workers = nullptr) {
    RunningServer running;
    running.server = std::make_unique<Server>(group.at(0), fiber::http::HttpHandler{}, workers);
    options.address = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 0);
    running.endpoint = running.server->add_endpoint<Http1Endpoint>(std::move(options));
    if (running.endpoint == nullptr || !running.server->start().has_value()) {
        running.server.reset();
        return running;
    }
    running.port = running.endpoint->local_addr().port();

    auto done = std::make_shared<std::promise<void>>();
    running.serve_done = done->get_future();
    Server *server = running.server.get();
    fiber::async::spawn(group.at(0), [server, done]() -> DetachedTask {
        co_await server->serve();
        done->set_value();
        co_return;
    });
    return running;
}

TEST(Http1EndpointTest, ServesRequestsAndKeepsTheConnectionAlive) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::atomic<int> handled{0};
    auto running = start_server(group, Http1Endpoint::Options{
                                               .handler =
                                                       [&handled](fiber::http::HttpExchange &exchange) {
                                                           handled.fetch_add(1, std::memory_order_relaxed);
                                                           return write_text(exchange, 200, "ok");
                                                       },
                                       });
    ASSERT_NE(running.server, nullptr);
    ASSERT_NE(running.port, 0);

    int client = connect_to(running.port);
    ASSERT_GE(client, 0);
    set_recv_timeout(client, 5s);

    // Two requests over one connection: the second proves keep-alive.
    ASSERT_TRUE(send_all(client, "GET /one HTTP/1.1\r\nHost: localhost\r\n\r\n"));
    std::string first = recv_until(client, "ok");
    EXPECT_NE(first.find("200"), std::string::npos);

    ASSERT_TRUE(send_all(client, "GET /two HTTP/1.1\r\nHost: localhost\r\n\r\n"));
    std::string second = recv_until(client, "ok");
    EXPECT_NE(second.find("200"), std::string::npos);
    EXPECT_EQ(handled.load(), 2);

    ::close(client);
    running.stop_and_join();
    EXPECT_EQ(running.server->state(), Server::State::Stopped);

    group.stop();
    group.join();
}

TEST(Http1EndpointTest, UsesTheServerDefaultHandlerWhenTheEndpointHasNone) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    RunningServer running;
    running.server = std::make_unique<Server>(
            group.at(0), [](fiber::http::HttpExchange &exchange) { return write_text(exchange, 200, "default"); });
    running.endpoint = running.server->add_endpoint<Http1Endpoint>(Http1Endpoint::Options{
            .address = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 0),
    });
    ASSERT_NE(running.endpoint, nullptr);
    ASSERT_TRUE(running.server->start().has_value());
    running.port = running.endpoint->local_addr().port();

    auto done = std::make_shared<std::promise<void>>();
    running.serve_done = done->get_future();
    Server *server = running.server.get();
    fiber::async::spawn(group.at(0), [server, done]() -> DetachedTask {
        co_await server->serve();
        done->set_value();
        co_return;
    });

    int client = connect_to(running.port);
    ASSERT_GE(client, 0);
    set_recv_timeout(client, 5s);
    ASSERT_TRUE(send_all(client, "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"));
    std::string response = recv_all(client);
    ::close(client);
    EXPECT_NE(response.find("default"), std::string::npos);

    running.stop_and_join();
    group.stop();
    group.join();
}

TEST(Http1EndpointTest, DrainClosesAnIdleKeepAliveConnection) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    auto running = start_server(
            group,
            Http1Endpoint::Options{
                    // A long keep-alive idle timeout: if drain did not wake the
                    // idle connection, this test would hang rather than fail fast.
                    .http1 = {.keep_alive_timeout = 300s},
                    .handler = [](fiber::http::HttpExchange &exchange) { return write_text(exchange, 200, "ok"); },
            });
    ASSERT_NE(running.server, nullptr);

    int client = connect_to(running.port);
    ASSERT_GE(client, 0);
    set_recv_timeout(client, 5s);
    ASSERT_TRUE(send_all(client, "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"));
    EXPECT_NE(recv_until(client, "ok").find("200"), std::string::npos);

    // The connection is now idle in keep-alive; stopping must close it
    // promptly instead of waiting out keep_alive_timeout.
    const auto began = std::chrono::steady_clock::now();
    running.stop_and_join();
    const auto elapsed = std::chrono::steady_clock::now() - began;
    EXPECT_LT(elapsed, 5s);

    // Server side is gone: the client read returns EOF.
    EXPECT_TRUE(recv_all(client).empty());
    ::close(client);

    group.stop();
    group.join();
}

TEST(Http1EndpointTest, DrainLetsAnInFlightRequestFinishAndMarksConnectionClose) {
    fiber::event::EventLoopGroup group(2);
    group.start();

    std::promise<void> in_handler;
    auto in_handler_future = in_handler.get_future();
    std::atomic<bool> entered{false};
    std::atomic<bool> completed{false};

    auto running = start_server(
            group, Http1Endpoint::Options{
                           .handler = [&](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
                               if (!entered.exchange(true)) {
                                   in_handler.set_value();
                               }
                               // Still running when stop() lands.
                               co_await fiber::async::sleep(300ms);
                               co_await write_text(exchange, 200, "late-but-complete");
                               completed.store(true);
                               co_return;
                           },
                   });
    ASSERT_NE(running.server, nullptr);

    int client = connect_to(running.port);
    ASSERT_GE(client, 0);
    set_recv_timeout(client, 10s);
    ASSERT_TRUE(send_all(client, "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"));

    ASSERT_EQ(in_handler_future.wait_for(5s), std::future_status::ready);
    running.server->stop(); // handler is mid-flight

    std::string response = recv_all(client);
    ::close(client);
    running.serve_done.get();

    EXPECT_TRUE(completed.load());
    EXPECT_NE(response.find("200"), std::string::npos);
    EXPECT_NE(response.find("late-but-complete"), std::string::npos);
    // A draining connection announces it will not be reused.
    EXPECT_NE(response.find("Connection: close"), std::string::npos);

    group.stop();
    group.join();
}

// Shutdown has no deadline and nothing forces a session down: a request still
// arriving after stop() runs to completion. This is the case the removed drain
// budget would have cut off.
TEST(Http1EndpointTest, DrainWaitsForASlowInFlightRequest) {
    fiber::event::EventLoopGroup group(2);
    group.start();

    std::promise<void> in_handler;
    auto in_handler_future = in_handler.get_future();
    std::atomic<bool> entered{false};
    std::atomic<bool> body_read{false};

    auto running = start_server(
            group, Http1Endpoint::Options{
                           .handler = [&](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
                               if (!entered.exchange(true)) {
                                   in_handler.set_value();
                               }
                               auto drained = co_await exchange.discard_body();
                               body_read.store(drained.has_value());
                               co_await write_text(exchange, 200, "complete");
                               co_return;
                           },
                   });
    ASSERT_NE(running.server, nullptr);

    int client = connect_to(running.port);
    ASSERT_GE(client, 0);
    set_recv_timeout(client, 10s);
    // Head only: the handler blocks reading a body that has not been sent.
    ASSERT_TRUE(send_all(client, "POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\n"));
    ASSERT_EQ(in_handler_future.wait_for(5s), std::future_status::ready);

    running.server->stop();
    // The connection is mid-request while the server drains; the rest of the
    // request only shows up well after that.
    std::this_thread::sleep_for(200ms);
    ASSERT_TRUE(send_all(client, "hello"));

    std::string response = recv_all(client);
    ::close(client);
    running.serve_done.get();

    EXPECT_TRUE(body_read.load());
    EXPECT_NE(response.find("200"), std::string::npos);
    EXPECT_NE(response.find("complete"), std::string::npos);
    EXPECT_NE(response.find("Connection: close"), std::string::npos);
    EXPECT_EQ(running.server->state(), Server::State::Stopped);

    group.stop();
    group.join();
}

TEST(Http1EndpointTest, ConnectionsSpreadOverWorkerLoops) {
    fiber::event::EventLoopGroup accept_group(1);
    fiber::event::EventLoopGroup workers(3);
    accept_group.start();
    workers.start();

    std::mutex loops_mu;
    std::set<fiber::event::EventLoop *> seen_loops;
    auto running = start_server(accept_group,
                                Http1Endpoint::Options{
                                        .handler =
                                                [&](fiber::http::HttpExchange &exchange) {
                                                    {
                                                        std::lock_guard guard(loops_mu);
                                                        seen_loops.insert(&fiber::event::EventLoop::current());
                                                    }
                                                    return write_text(exchange, 200, "ok");
                                                },
                                },
                                &workers);
    ASSERT_NE(running.server, nullptr);
    EXPECT_EQ(running.server->worker_count(), 3u);

    // Round-robin over three workers: keep all three connections open at once
    // so each lands on a different loop.
    std::vector<int> clients;
    for (int i = 0; i < 3; ++i) {
        int client = connect_to(running.port);
        ASSERT_GE(client, 0);
        set_recv_timeout(client, 5s);
        ASSERT_TRUE(send_all(client, "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"));
        EXPECT_NE(recv_until(client, "ok").find("200"), std::string::npos);
        clients.push_back(client);
    }

    {
        std::lock_guard guard(loops_mu);
        EXPECT_EQ(seen_loops.size(), 3u);
        for (fiber::event::EventLoop *loop: seen_loops) {
            EXPECT_NE(loop, &accept_group.at(0));
        }
    }

    running.stop_and_join();
    for (int client: clients) {
        ::close(client);
    }

    accept_group.stop();
    accept_group.join();
    workers.stop();
    workers.join();
}

TEST(Http1EndpointTest, ConnectionsAfterStopAreRefused) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    auto running = start_server(
            group,
            Http1Endpoint::Options{
                    .handler = [](fiber::http::HttpExchange &exchange) { return write_text(exchange, 200, "ok"); },
            });
    ASSERT_NE(running.server, nullptr);
    const std::uint16_t port = running.port;
    running.stop_and_join();

    // The listener is closed, so a fresh connection either fails outright or
    // is reset without a response.
    int client = connect_to(port);
    if (client >= 0) {
        set_recv_timeout(client, 2s);
        (void) send_all(client, "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
        EXPECT_TRUE(recv_all(client).empty());
        ::close(client);
    }

    group.stop();
    group.join();
}


TEST(Http1EndpointTest, DrainWaitsForRequestBodyAndHandlerLifetime) {
    fiber::event::EventLoopGroup group(2);
    group.start();
    std::promise<void> reading_body;
    auto reading = reading_body.get_future();
    auto lifetime = std::make_shared<int>(1);
    std::weak_ptr<int> weak = lifetime;
    Http1Endpoint::Options options{};
    options.handler = [&, lifetime](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        reading_body.set_value();
        auto body = co_await exchange.read_body(16);
        EXPECT_TRUE(body);
        if (body) {
            co_await write_text(exchange, 200, "body received");
        }
    };
    auto running = start_server(group, std::move(options), &group);
    lifetime.reset();
    ASSERT_TRUE(running.server);
    int fd = connect_to(running.port);
    ASSERT_GE(fd, 0);
    set_recv_timeout(fd, 3s);
    EXPECT_TRUE(send_all(fd, "POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\n\r\n"));
    EXPECT_EQ(reading.wait_for(3s), std::future_status::ready);
    running.server->stop();
    EXPECT_EQ(running.serve_done.wait_for(200ms), std::future_status::timeout);
    EXPECT_FALSE(weak.expired());
    EXPECT_TRUE(send_all(fd, "body"));
    auto response = recv_all(fd);
    EXPECT_NE(response.find("body received"), std::string::npos);
    EXPECT_NE(response.find("Connection: close"), std::string::npos);
    ::close(fd);
    running.serve_done.get();
    EXPECT_TRUE(weak.expired());
    group.stop();
    group.join();
}

// Counts this process's open descriptors (minus the DIR's own fd).
int count_open_fds() {
    DIR *dir = ::opendir("/proc/self/fd");
    if (dir == nullptr) {
        return -1;
    }
    int count = 0;
    while (::readdir(dir) != nullptr) {
        ++count;
    }
    ::closedir(dir);
    return count - 1;
}

// §13-G of the rewrite design: hammer accept with short-lived connections
// while stop() lands off-loop, then prove the accounting held. A connection
// that slipped past a destroyed worker would outlive serve() and show up as a
// leaked descriptor; a drain that cut a response short would show up as a
// truncated read.
TEST(Http1EndpointTest, ConcurrentAcceptAndStopReleasesEveryConnection) {
    fiber::event::EventLoopGroup group(3);
    group.start();

    constexpr std::string_view kBody = "complete";
    Http1Endpoint::Options options{};
    options.handler = [](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        co_await fiber::async::sleep(1ms);
        co_await write_text(exchange, 200, "complete");
    };

    // Warmup round: one full request/response so lazily-created resources
    // exist before the descriptor baseline is taken.
    {
        auto warmup = start_server(group, options, &group);
        ASSERT_TRUE(warmup.server);
        int fd = connect_to(warmup.port);
        ASSERT_GE(fd, 0);
        set_recv_timeout(fd, 5s);
        ASSERT_TRUE(send_all(fd, "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"));
        std::string response = recv_all(fd);
        ::close(fd);
        ASSERT_TRUE(response.size() >= kBody.size() &&
                    std::equal(kBody.begin(), kBody.end(), response.end() - kBody.size()));
        warmup.stop_and_join();
    }
    const int fd_baseline = count_open_fds();
    ASSERT_GE(fd_baseline, 0);
    int total_served = 0;

    for (int iteration = 0; iteration < 12; ++iteration) {
        auto running = start_server(group, options, &group);
        ASSERT_TRUE(running.server);
        std::atomic<bool> stopping{false};
        std::atomic<int> attempts{0};
        std::atomic<int> served{0};
        std::atomic<int> dropped{0};
        std::atomic<int> truncated{0};
        std::vector<std::thread> clients;
        for (int i = 0; i < 4; ++i) {
            clients.emplace_back([&] {
                while (!stopping.load()) {
                    attempts.fetch_add(1, std::memory_order_relaxed);
                    int fd = connect_to(running.port);
                    if (fd < 0) {
                        continue;
                    }
                    set_recv_timeout(fd, 2s);
                    constexpr std::string_view request =
                            "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
                    (void) ::send(fd, request.data(), request.size(), MSG_NOSIGNAL);
                    std::string response = recv_all(fd);
                    ::close(fd);
                    // Drain may drop a connection before its request is ever
                    // read, but it must never cut one off mid-response.
                    if (response.empty()) {
                        dropped.fetch_add(1, std::memory_order_relaxed);
                    } else if (response.size() >= kBody.size() &&
                               std::equal(kBody.begin(), kBody.end(), response.end() - kBody.size())) {
                        served.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        truncated.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        while (attempts.load() < 8) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2 + 3 * (iteration % 5)));
        running.server->stop(); // off the owner loop, racing the accept path
        stopping.store(true);
        for (auto &client: clients) {
            client.join();
        }
        EXPECT_EQ(running.serve_done.wait_for(10s), std::future_status::ready);
        running.serve_done.get();
        EXPECT_EQ(running.server->state(), Server::State::Stopped);
        EXPECT_EQ(truncated.load(), 0);
        EXPECT_GT(served.load() + dropped.load(), 0);
        total_served += served.load();
        // serve() returning means every connection ended; give the last
        // descriptors a beat to leave /proc/self/fd and require no growth.
        std::this_thread::sleep_for(20ms);
        EXPECT_EQ(count_open_fds(), fd_baseline);
    }
    // Across the rounds the hammer genuinely exchanged traffic, so the
    // no-truncation check above was not vacuous.
    EXPECT_GT(total_served, 0);

    group.stop();
    group.join();
}

} // namespace
