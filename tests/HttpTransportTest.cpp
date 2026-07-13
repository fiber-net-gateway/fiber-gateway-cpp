#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

#include "async/Spawn.h"
#include "common/IoError.h"
#include "event/EventLoopGroup.h"
#include "http/HttpTransport.h"
#include "net/SocketAddress.h"
#include "net/TcpListener.h"

namespace {

using namespace std::chrono_literals;

fiber::async::DetachedTask wait_on_tcp_transport(fiber::event::EventLoop *loop, int fd,
                                                 std::chrono::milliseconds timeout,
                                                 std::promise<fiber::common::IoResult<void>> *done) {
    fiber::net::SocketAddress peer(fiber::net::IpAddress::loopback_v4(), 0);
    auto transport_result = fiber::http::TcpTransport::create(*loop, fiber::net::AcceptResult(fd, peer));
    if (!transport_result) {
        done->set_value(std::unexpected(transport_result.error()));
        co_return;
    }

    auto transport = std::move(*transport_result);
    auto wait_result = co_await transport->wait_readable(timeout);
    transport->close();
    done->set_value(std::move(wait_result));
    co_return;
}

fiber::common::IoResult<void> run_tcp_wait(int transport_fd, std::chrono::milliseconds timeout) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<fiber::common::IoResult<void>> done_promise;
    auto done_future = done_promise.get_future();
    fiber::async::spawn(group.at(0),
                        [&]() { return wait_on_tcp_transport(&group.at(0), transport_fd, timeout, &done_promise); });

    if (done_future.wait_for(2s) != std::future_status::ready) {
        group.stop();
        group.join();
        return std::unexpected(fiber::common::IoErr::TimedOut);
    }
    auto result = done_future.get();
    group.stop();
    group.join();
    return result;
}

TEST(HttpTransportTest, TcpWaitReadableTimesOutWhileSocketIsIdle) {
    int fds[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds), 0);

    int transport_fd = fds[0];
    fds[0] = -1;
    auto result = run_tcp_wait(transport_fd, 20ms);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), fiber::common::IoErr::TimedOut);
    ::close(fds[1]);
}

TEST(HttpTransportTest, TcpWaitReadableSucceedsWhenDataIsQueued) {
    int fds[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds), 0);
    ASSERT_EQ(::send(fds[1], "x", 1, 0), 1);

    int transport_fd = fds[0];
    fds[0] = -1;
    auto result = run_tcp_wait(transport_fd, 1s);

    EXPECT_TRUE(result);
    ::close(fds[1]);
}

} // namespace
