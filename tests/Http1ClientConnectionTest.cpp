#include <gtest/gtest.h>

#include <chrono>
#include <future>

#include "async/Spawn.h"
#include "common/IoError.h"
#include "event/EventLoopGroup.h"
#include "http/Http1ClientConnection.h"
#include "net/TcpListener.h"

namespace {

using namespace std::chrono_literals;
using fiber::async::DetachedTask;

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

DetachedTask run_hold_server(fiber::event::EventLoop *loop, std::promise<std::uint16_t> *port_promise,
                             std::promise<void> *done_promise) {
    fiber::net::TcpListener listener(*loop);
    fiber::net::ListenOptions listen_options{};
    auto bind_result =
            listener.bind(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 0), listen_options);
    if (!bind_result) {
        port_promise->set_value(0);
        done_promise->set_value();
        co_return;
    }

    auto port_result = resolve_port(listener.fd());
    port_promise->set_value(port_result ? *port_result : 0);

    auto accept_result = co_await listener.accept();
    (void) accept_result;
    listener.close();
    done_promise->set_value();
}

DetachedTask run_client_connect(fiber::event::EventLoop *loop, std::uint16_t port,
                                std::promise<fiber::common::IoErr> *result_promise,
                                std::promise<bool> *connected_promise) {
    fiber::http::Http1ClientConnectionOptions options;
    options.peer_addr = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port);
    options.tls.enabled = false;

    fiber::http::Http1ClientConnection connection(*loop, std::move(options));
    auto connect_result = co_await connection.connect();
    connected_promise->set_value(connect_result.has_value() && connection.connected() && connection.idle() &&
                                 connection.reusable() && !connection.busy());
    result_promise->set_value(connect_result ? fiber::common::IoErr::None : connect_result.error());
    connection.close();
}

TEST(Http1ClientConnectionTest, ConnectTransitionsToIdleReusableConnection) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<std::uint16_t> port_promise;
    auto port_future = port_promise.get_future();
    std::promise<void> server_done_promise;
    auto server_done_future = server_done_promise.get_future();
    fiber::async::spawn(group.at(0),
                        [&]() { return run_hold_server(&group.at(0), &port_promise, &server_done_promise); });

    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    std::promise<fiber::common::IoErr> result_promise;
    std::promise<bool> connected_promise;
    auto result_future = result_promise.get_future();
    auto connected_future = connected_promise.get_future();
    fiber::async::spawn(group.at(0),
                        [&]() { return run_client_connect(&group.at(0), port, &result_promise, &connected_promise); });

    EXPECT_TRUE(connected_future.get());
    EXPECT_EQ(result_future.get(), fiber::common::IoErr::None);

    server_done_future.get();
    group.stop();
    group.join();
}

} // namespace
