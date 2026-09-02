#include <gtest/gtest.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <future>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

#include <fiber/async/Spawn.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/HttpServer.h>
#include <fiber/net/SocketAddress.h>

namespace {

using fiber::async::DetachedTask;
using namespace std::chrono_literals;

struct BindObservation {
    fiber::common::IoErr error = fiber::common::IoErr::None;
    int fd = -1;
    fiber::http::HttpServer::State state = fiber::http::HttpServer::State::Created;
};

fiber::common::IoResult<std::uint16_t> resolve_port(int fd) {
    sockaddr_storage storage{};
    socklen_t length = sizeof(storage);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&storage), &length) != 0) {
        return std::unexpected(fiber::common::io_err_from_errno(errno));
    }
    fiber::net::SocketAddress address;
    if (!fiber::net::SocketAddress::from_sockaddr(reinterpret_cast<const sockaddr *>(&storage), length, address)) {
        return std::unexpected(fiber::common::IoErr::NotSupported);
    }
    return address.port();
}

TEST(HttpServerLifecycleTest, FailedHttp3WithoutTlsBindRollsBackListener) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<BindObservation> observation_promise;
    auto observation_future = observation_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() -> DetachedTask {
        fiber::http::HttpServerOptions options;
        options.http3.enabled = true;
        fiber::http::HttpServer server(group.at(0), {}, std::move(options));

        auto result = server.bind({fiber::net::IpAddress::loopback_v4(), 0}, {});
        BindObservation observation;
        observation.error = result ? fiber::common::IoErr::None : result.error();
        observation.fd = server.fd();
        observation.state = server.state();
        co_await server.shutdown_and_wait();
        observation_promise.set_value(observation);
        co_return;
    });

    BindObservation observation = observation_future.get();
    group.stop();
    group.join();

    EXPECT_NE(observation.error, fiber::common::IoErr::None);
    EXPECT_EQ(observation.fd, -1);
    EXPECT_EQ(observation.state, fiber::http::HttpServer::State::Created);
}

TEST(HttpServerLifecycleTest, RequestCloseCanBeIssuedOffOwnerLoop) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::http::HttpServer server(group.at(0), {});
    std::promise<bool> bound_promise;
    auto bound_future = bound_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() -> DetachedTask {
        auto result = server.bind({fiber::net::IpAddress::loopback_v4(), 0}, {});
        if (result) {
            fiber::async::spawn(group.at(0), [&]() { return server.serve(); });
        }
        bound_promise.set_value(result.has_value());
        co_return;
    });

    EXPECT_TRUE(bound_future.get());
    server.request_close();

    std::promise<fiber::http::HttpServer::State> closed_promise;
    auto closed_future = closed_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() -> DetachedTask {
        co_await server.shutdown_and_wait();
        closed_promise.set_value(server.state());
        co_return;
    });

    const bool closed = closed_future.wait_for(5s) == std::future_status::ready;
    EXPECT_TRUE(closed);
    if (closed) {
        EXPECT_EQ(closed_future.get(), fiber::http::HttpServer::State::Closed);
    }
    group.stop();
    group.join();
    EXPECT_EQ(server.fd(), -1);
}

TEST(HttpServerLifecycleTest, ShutdownClosesAnIdleHttp1Connection) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<void> request_handled_promise;
    auto request_handled_future = request_handled_promise.get_future();
    fiber::http::HttpHandler handler =
            [&request_handled_promise](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        auto result = co_await exchange.send_header({
                .kind = fiber::http::OutgoingHeaderKind::Final,
                .status_code = 204,
                .body = fiber::http::HttpBodySpec::None(),
                .end_stream = true,
        });
        if (result) {
            request_handled_promise.set_value();
        }
        co_return;
    };
    fiber::http::HttpServer server(group.at(0), std::move(handler));

    std::promise<std::uint16_t> port_promise;
    auto port_future = port_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() -> DetachedTask {
        auto result = server.bind({fiber::net::IpAddress::loopback_v4(), 0}, {});
        if (!result) {
            port_promise.set_value(0);
            co_return;
        }
        auto port = resolve_port(server.fd());
        port_promise.set_value(port ? *port : 0);
        fiber::async::spawn(group.at(0), [&]() { return server.serve(); });
        co_return;
    });

    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);
    const int client = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    ASSERT_GE(client, 0);
    fiber::net::SocketAddress address(fiber::net::IpAddress::loopback_v4(), port);
    sockaddr_storage storage{};
    socklen_t length = 0;
    ASSERT_TRUE(address.to_sockaddr(storage, length));
    ASSERT_EQ(::connect(client, reinterpret_cast<sockaddr *>(&storage), length), 0);
    constexpr char request[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ASSERT_EQ(::send(client, request, sizeof(request) - 1, 0), static_cast<ssize_t>(sizeof(request) - 1));
    ASSERT_EQ(request_handled_future.wait_for(5s), std::future_status::ready);

    std::promise<void> closed_promise;
    auto closed_future = closed_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() -> DetachedTask {
        co_await server.shutdown_and_wait();
        closed_promise.set_value();
        co_return;
    });
    EXPECT_EQ(closed_future.wait_for(5s), std::future_status::ready);
    ::close(client);
    group.stop();
    group.join();
}

} // namespace
