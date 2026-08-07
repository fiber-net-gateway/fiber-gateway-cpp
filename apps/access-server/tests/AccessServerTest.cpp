#include "../src/runtime/AccessServer.h"

#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <future>
#include <string>
#include <thread>
#include <utility>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <fiber/async/Spawn.h>
#include <fiber/cat/CatClient.h>
#include <fiber/cat/CatClientConfig.h>
#include <fiber/event/EventLoop.h>
#include <fiber/event/EventLoopGroup.h>

namespace fiber::access_server {
namespace {

using namespace std::chrono_literals;

std::uint16_t listener_port(int fd) {
    sockaddr_in address{};
    socklen_t length = sizeof(address);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&address), &length) != 0) {
        return 0;
    }
    return ntohs(address.sin_port);
}

std::string request(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return {};
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return {};
    }

    constexpr std::string_view payload = "GET / HTTP/1.1\r\n"
                                         "Host: api.example.com\r\n"
                                         "Connection: close\r\n\r\n";
    std::size_t sent = 0;
    while (sent < payload.size()) {
        const ssize_t size = ::send(fd, payload.data() + sent, payload.size() - sent, 0);
        if (size <= 0) {
            ::close(fd);
            return {};
        }
        sent += static_cast<std::size_t>(size);
    }

    std::string response;
    std::array<char, 4096> buffer{};
    for (;;) {
        const ssize_t size = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (size == 0) {
            break;
        }
        if (size < 0) {
            if (errno == EINTR) {
                continue;
            }
            response.clear();
            break;
        }
        response.append(buffer.data(), static_cast<std::size_t>(size));
    }
    ::close(fd);
    return response;
}

ProjectConfig response_config() {
    ProjectConfig config;
    config.version = 1;
    config.hosts = std::vector<HostConfigEntry>{
            HostConfigEntry{
                    .pattern = "api.example.com",
                    .strategy = HostStrategyConfig{},
            },
    };
    RouteConfig route;
    route.path = "/";
    route.type = RouteType::Response;
    route.status = 200;
    route.body = RouteBodyConfig{
            .type = BodyType::Text,
            .content = "ok",
    };
    config.routes = std::vector<std::optional<RouteConfig>>{std::move(route)};
    return config;
}

TEST(AccessServerTest, ServesPublishedSnapshotAndShutsDownWorkerResources) {
    RouteConfigStore store;
    auto published = store.apply("demo", response_config());
    ASSERT_TRUE(published);

    event::EventLoop accept_loop;
    event::EventLoopGroup workers(1);
    AccessServer server(accept_loop, workers, store, {});
    std::promise<std::pair<std::uint16_t, std::uint16_t>> port_promise;
    auto port = port_promise.get_future();
    std::promise<void> stopped_promise;
    auto stopped = stopped_promise.get_future();
    bool startup_ok = false;

    workers.start();
    async::spawn(accept_loop, [&]() -> async::DetachedTask {
        auto initialized = server.initialize();
        if (!initialized) {
            port_promise.set_value({0, 0});
            accept_loop.stop();
            co_return;
        }
        auto loopback = net::IpAddress::v4({127, 0, 0, 1});
        auto bound = server.bind(net::SocketAddress(loopback, 0));
        if (!bound) {
            port_promise.set_value({0, 0});
            co_await server.shutdown_and_wait();
            accept_loop.stop();
            co_return;
        }
        auto metrics_bound = server.bind_metrics(net::SocketAddress(loopback, 0));
        if (!metrics_bound) {
            port_promise.set_value({0, 0});
            co_await server.shutdown_and_wait();
            accept_loop.stop();
            co_return;
        }
        startup_ok = true;
        port_promise.set_value({listener_port(server.fd()), listener_port(server.metrics_fd())});
        async::spawn([&server]() { return server.serve(); });
        async::spawn([&server]() { return server.serve_metrics(); });
    });

    std::string response;
    std::string metrics_response;
    std::thread client([&]() {
        const auto [bound_port, metrics_port] = port.get();
        if (bound_port != 0 && metrics_port != 0) {
            response = request(bound_port);
            metrics_response = request(metrics_port);
        }
        async::spawn(accept_loop, [&]() -> async::DetachedTask {
            if (startup_ok) {
                co_await server.shutdown_and_wait();
            }
            stopped_promise.set_value();
            accept_loop.stop();
        });
    });

    accept_loop.run();
    client.join();
    EXPECT_EQ(stopped.wait_for(2s), std::future_status::ready);
    workers.stop();
    workers.join();

    ASSERT_TRUE(startup_ok);
    EXPECT_NE(response.find("HTTP/1.1 200"), std::string::npos);
    EXPECT_TRUE(response.ends_with("\r\n\r\nok"));
    EXPECT_NE(metrics_response.find("HTTP/1.1 200"), std::string::npos);
    EXPECT_NE(metrics_response.find("access_server_requests_total{result=\"success\"} 1"), std::string::npos);
    EXPECT_NE(metrics_response.find("access_server_request_duration_seconds_count 1"), std::string::npos);
}

TEST(AccessServerTest, ReturnsCatTraceIdFromTheUnifiedRequestContext) {
    RouteConfigStore store;
    auto published = store.apply("demo", response_config());
    ASSERT_TRUE(published);

    event::EventLoop accept_loop;
    event::EventLoopGroup workers(1);
    event::EventLoopGroup cat_group(1);
    cat::CatClientConfigParams cat_params{
            .app_key = "unified-access-server",
            .hostname = "access-test",
            .ip = "127.0.0.1",
            .thread_group_name = "access-test-cat",
            .thread_id = "0",
            .thread_name = "cat-sender",
            .bootstrap_collectors =
                    {
                            net::SocketAddress(net::IpAddress::v4({127, 0, 0, 1}), 1),
                    },
    };
    auto cat_config = cat::CatClientConfig::create(std::move(cat_params));
    ASSERT_TRUE(cat_config);
    cat::CatClientOptions cat_options;
    cat_options.enable_heartbeat = false;
    cat_options.enable_system_stats = false;
    cat_options.shutdown_drain_timeout = 10ms;
    auto cat_client = cat::CatClient::create(cat_group.at(0), std::move(*cat_config), cat_options);
    ASSERT_TRUE(cat_client);

    AccessServer server(accept_loop, workers, store, {},
                        AccessServerOptions{
                                .cat_client = cat_client->get(),
                        });
    std::promise<bool> cat_started_promise;
    auto cat_started = cat_started_promise.get_future();
    std::promise<std::uint16_t> port_promise;
    auto port = port_promise.get_future();
    std::promise<void> stopped_promise;
    auto stopped = stopped_promise.get_future();
    bool startup_ok = false;

    workers.start();
    cat_group.start();
    async::spawn(cat_group.at(0), [&]() -> async::DetachedTask {
        cat_started_promise.set_value((*cat_client)->start().has_value());
        co_return;
    });
    ASSERT_EQ(cat_started.wait_for(2s), std::future_status::ready);
    ASSERT_TRUE(cat_started.get());

    async::spawn(accept_loop, [&]() -> async::DetachedTask {
        auto initialized = server.initialize();
        if (!initialized) {
            port_promise.set_value(0);
            accept_loop.stop();
            co_return;
        }
        auto loopback = net::IpAddress::v4({127, 0, 0, 1});
        auto bound = server.bind(net::SocketAddress(loopback, 0));
        if (!bound) {
            port_promise.set_value(0);
            co_await server.shutdown_and_wait();
            accept_loop.stop();
            co_return;
        }
        startup_ok = true;
        port_promise.set_value(listener_port(server.fd()));
        async::spawn([&server]() { return server.serve(); });
    });

    std::string response;
    std::thread client([&]() {
        const std::uint16_t bound_port = port.get();
        if (bound_port != 0) {
            response = request(bound_port);
        }
        async::spawn(accept_loop, [&]() -> async::DetachedTask {
            if (startup_ok) {
                co_await server.shutdown_and_wait();
            }
            stopped_promise.set_value();
            accept_loop.stop();
        });
    });

    accept_loop.run();
    client.join();
    EXPECT_EQ(stopped.wait_for(2s), std::future_status::ready);

    std::promise<void> cat_stopped_promise;
    auto cat_stopped = cat_stopped_promise.get_future();
    async::spawn(cat_group.at(0), [&]() -> async::DetachedTask {
        co_await (*cat_client)->shutdown();
        cat_stopped_promise.set_value();
    });
    ASSERT_EQ(cat_stopped.wait_for(2s), std::future_status::ready);
    workers.stop();
    cat_group.stop();
    workers.join();
    cat_group.join();

    ASSERT_TRUE(startup_ok);
    const std::size_t trace = response.find("Hi-Trace-Id: ");
    ASSERT_NE(trace, std::string::npos);
    const std::size_t trace_end = response.find("\r\n", trace);
    ASSERT_NE(trace_end, std::string::npos);
    EXPECT_GT(trace_end, trace + std::string_view("Hi-Trace-Id: ").size());
}

} // namespace
} // namespace fiber::access_server
