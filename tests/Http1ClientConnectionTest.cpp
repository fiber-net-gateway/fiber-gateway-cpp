#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <csignal>
#include <future>
#include <new>
#include <span>
#include <sys/wait.h>
#include <unistd.h>

#include <fiber/async/Spawn.h>
#include <fiber/common/IoError.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/Http1ClientConnection.h>
#include <fiber/net/TcpListener.h>

namespace {

using namespace std::chrono_literals;
using fiber::async::DetachedTask;

enum class WrongThreadOperation {
    Close,
    Destroy,
};

int run_wrong_thread_operation_child(WrongThreadOperation operation) {
    pid_t pid = ::fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        fiber::event::EventLoopGroup group(1);
        auto *connection = new (std::nothrow) fiber::http::Http1ClientConnection(group.at(0));
        if (!connection) {
            _exit(10);
        }
        if (operation == WrongThreadOperation::Close) {
            connection->close();
        } else {
            delete connection;
        }
        _exit(11);
    }

    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) {
        return -2;
    }
    return WIFSIGNALED(status) ? WTERMSIG(status) : 0;
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
    fiber::http::Http1ClientConnection connection(*loop);
    auto connect_result =
            co_await connection.connect(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port), 5s);
    connected_promise->set_value(connect_result.has_value() && connection.connected() && connection.idle() &&
                                 connection.reusable() && !connection.busy() && connection.request_count() == 0);
    result_promise->set_value(connect_result ? fiber::common::IoErr::None : connect_result.error());
    connection.close();
}

DetachedTask run_client_multi_connect(fiber::event::EventLoop *loop, std::uint16_t port,
                                      std::promise<fiber::common::IoErr> *result_promise,
                                      std::promise<bool> *connected_promise) {
    std::array<fiber::net::SocketAddress, 2> addresses{{
            fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v6(), port),
            fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port),
    }};
    fiber::http::Http1ClientConnection connection(*loop);

    fiber::net::HappyEyeballsOptions connect_options;
    connect_options.total_timeout = 5s;
    auto connect_result = co_await connection.connect(addresses, connect_options);
    connected_promise->set_value(connect_result.has_value() && connection.connected() && connection.idle() &&
                                 connection.reusable() && connection.peer_addr().family() == fiber::net::IpFamily::V4);
    result_promise->set_value(connect_result ? fiber::common::IoErr::None : connect_result.error());
    connection.close();
}

DetachedTask run_client_connect_after_failed_multi(fiber::event::EventLoop *loop, std::uint16_t port,
                                                   std::promise<fiber::common::IoErr> *result_promise,
                                                   std::promise<bool> *state_promise) {
    const fiber::net::SocketAddress peer(fiber::net::IpAddress::loopback_v4(), port);
    fiber::http::Http1ClientConnection connection(*loop);

    fiber::net::HappyEyeballsOptions connect_options;
    connect_options.total_timeout = 5s;
    auto failed_result = co_await connection.connect(std::span<const fiber::net::SocketAddress>{}, connect_options);
    const bool failure_left_no_partial_state =
            !failed_result && failed_result.error() == fiber::common::IoErr::NotFound && !connection.valid() &&
            !connection.connected() && !connection.idle() && !connection.reusable();

    auto connect_result = co_await connection.connect(peer, 5s);
    state_promise->set_value(failure_left_no_partial_state && connect_result.has_value() && connection.connected() &&
                             connection.peer_addr().family() == fiber::net::IpFamily::V4);
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

TEST(Http1ClientConnectionTest, MultiAddressConnectPublishesOnlyTheWinningPeer) {
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
    fiber::async::spawn(group.at(0), [&]() {
        return run_client_multi_connect(&group.at(0), port, &result_promise, &connected_promise);
    });

    EXPECT_TRUE(connected_future.get());
    EXPECT_EQ(result_future.get(), fiber::common::IoErr::None);

    server_done_future.get();
    group.stop();
    group.join();
}

TEST(Http1ClientConnectionTest, FailedMultiAddressConnectLeavesNoPartialStateAndCanRetry) {
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
    std::promise<bool> state_promise;
    auto result_future = result_promise.get_future();
    auto state_future = state_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_client_connect_after_failed_multi(&group.at(0), port, &result_promise, &state_promise);
    });

    EXPECT_TRUE(state_future.get());
    EXPECT_EQ(result_future.get(), fiber::common::IoErr::None);

    server_done_future.get();
    group.stop();
    group.join();
}

TEST(Http1ClientConnectionTest, CloseOutsideOwnerLoopAborts) {
    EXPECT_EQ(run_wrong_thread_operation_child(WrongThreadOperation::Close), SIGABRT);
}

TEST(Http1ClientConnectionTest, DestructionOutsideOwnerLoopAbortsEvenWhenNotConnected) {
    EXPECT_EQ(run_wrong_thread_operation_child(WrongThreadOperation::Destroy), SIGABRT);
}

} // namespace
