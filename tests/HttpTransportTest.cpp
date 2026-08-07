#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <future>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/common/IoError.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/HttpTransport.h>
#include <fiber/net/SocketAddress.h>
#include <fiber/net/TcpListener.h>

namespace {

using namespace std::chrono_literals;

bool create_connected_tcp_sockets(int &server_fd, int &client_fd) {
    server_fd = -1;
    client_fd = -1;
    int listener_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener_fd < 0) {
        return false;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(listener_fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0 ||
        ::listen(listener_fd, 1) != 0) {
        ::close(listener_fd);
        return false;
    }
    socklen_t address_len = sizeof(address);
    if (::getsockname(listener_fd, reinterpret_cast<sockaddr *>(&address), &address_len) != 0) {
        ::close(listener_fd);
        return false;
    }
    client_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (client_fd < 0 || ::connect(client_fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0) {
        if (client_fd >= 0) {
            ::close(client_fd);
            client_fd = -1;
        }
        ::close(listener_fd);
        return false;
    }
    server_fd = ::accept4(listener_fd, nullptr, nullptr, SOCK_CLOEXEC);
    ::close(listener_fd);
    if (server_fd < 0) {
        ::close(client_fd);
        client_fd = -1;
        return false;
    }
    return true;
}

struct TcpNoDelayResult {
    fiber::common::IoErr error = fiber::common::IoErr::Invalid;
    int value = -1;
};

fiber::async::DetachedTask inspect_tcp_no_delay(fiber::event::EventLoop *loop, int fd,
                                                fiber::net::TcpSocketOptions options,
                                                std::promise<TcpNoDelayResult> *done) {
    TcpNoDelayResult result;
    fiber::net::SocketAddress peer(fiber::net::IpAddress::loopback_v4(), 0);
    auto transport_result = fiber::http::TcpTransport::create(*loop, fiber::net::AcceptResult(fd, peer), options);
    if (!transport_result) {
        result.error = transport_result.error();
        done->set_value(result);
        co_return;
    }
    auto transport = std::move(*transport_result);
    socklen_t value_len = sizeof(result.value);
    if (::getsockopt(transport->fd(), IPPROTO_TCP, TCP_NODELAY, &result.value, &value_len) == 0) {
        result.error = fiber::common::IoErr::None;
    } else {
        result.error = fiber::common::io_err_from_errno(errno);
    }
    transport->close();
    done->set_value(result);
    co_return;
}

TcpNoDelayResult run_tcp_no_delay_check(int fd, fiber::net::TcpSocketOptions options) {
    fiber::event::EventLoopGroup group(1);
    group.start();
    std::promise<TcpNoDelayResult> done_promise;
    auto done_future = done_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return inspect_tcp_no_delay(&group.at(0), fd, options, &done_promise); });
    TcpNoDelayResult result;
    if (done_future.wait_for(2s) == std::future_status::ready) {
        result = done_future.get();
    } else {
        result.error = fiber::common::IoErr::TimedOut;
    }
    group.stop();
    group.join();
    return result;
}

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

struct TcpCallbackResult {
    fiber::common::IoErr initial_poll_err = fiber::common::IoErr::Invalid;
    fiber::event::IoEvent initial_wait_event = fiber::event::IoEvent::None;
    fiber::common::IoErr set_err = fiber::common::IoErr::Invalid;
    fiber::common::IoErr callback_err = fiber::common::IoErr::Invalid;
    fiber::common::IoErr clear_err = fiber::common::IoErr::Invalid;
    fiber::common::IoErr callback_poll_err = fiber::common::IoErr::Invalid;
    fiber::event::IoEvent callback_wait_event = fiber::event::IoEvent::None;
    std::array<char, 16> data{};
    std::size_t read = 0;
    bool called = false;
};

struct TcpCallbackContext {
    fiber::http::TcpTransport *transport = nullptr;
    TcpCallbackResult *result = nullptr;
};

void on_tcp_transport_readable(void *opaque, fiber::common::IoErr err) noexcept {
    auto *ctx = static_cast<TcpCallbackContext *>(opaque);
    ctx->result->callback_err = err;
    ctx->result->called = true;
    if (err != fiber::common::IoErr::None) {
        return;
    }

    ctx->result->clear_err = ctx->transport->clear_read_callback(&on_tcp_transport_readable, ctx);
    ctx->result->callback_poll_err = ctx->transport->poll_read(ctx->result->data.data(), ctx->result->data.size(),
                                                               ctx->result->read, ctx->result->callback_wait_event);
}

fiber::async::DetachedTask exercise_tcp_callback(fiber::event::EventLoop *loop, int fd, int peer_fd,
                                                 std::promise<TcpCallbackResult> *done) {
    TcpCallbackResult result;
    fiber::net::SocketAddress peer(fiber::net::IpAddress::loopback_v4(), 0);
    auto transport_result = fiber::http::TcpTransport::create(*loop, fiber::net::AcceptResult(fd, peer));
    if (!transport_result) {
        result.initial_poll_err = transport_result.error();
        done->set_value(result);
        co_return;
    }

    auto transport = std::move(*transport_result);
    result.initial_poll_err =
            transport->poll_read(result.data.data(), result.data.size(), result.read, result.initial_wait_event);
    TcpCallbackContext ctx{transport.get(), &result};
    result.set_err = transport->set_read_callback(&on_tcp_transport_readable, &ctx);
    if (result.set_err == fiber::common::IoErr::None) {
        (void) ::send(peer_fd, "hello", 5, 0);
        for (int i = 0; i < 1000 && !result.called; ++i) {
            co_await fiber::async::sleep(1ms);
        }
        if (!result.called) {
            result.clear_err = transport->clear_read_callback(&on_tcp_transport_readable, &ctx);
        }
    }
    transport->close();
    done->set_value(result);
    co_return;
}

struct TcpBusyResult {
    fiber::common::IoErr set_err = fiber::common::IoErr::Invalid;
    fiber::common::IoResult<void> wait_result = std::unexpected(fiber::common::IoErr::Invalid);
    fiber::common::IoErr callback_err = fiber::common::IoErr::Invalid;
    std::size_t callback_calls = 0;
};

void record_tcp_callback(void *opaque, fiber::common::IoErr err) noexcept {
    auto *result = static_cast<TcpBusyResult *>(opaque);
    result->callback_err = err;
    ++result->callback_calls;
}

fiber::async::DetachedTask exercise_tcp_callback_busy(fiber::event::EventLoop *loop, int fd,
                                                      std::promise<TcpBusyResult> *done) {
    TcpBusyResult result;
    fiber::net::SocketAddress peer(fiber::net::IpAddress::loopback_v4(), 0);
    auto transport_result = fiber::http::TcpTransport::create(*loop, fiber::net::AcceptResult(fd, peer));
    if (!transport_result) {
        result.set_err = transport_result.error();
        done->set_value(std::move(result));
        co_return;
    }

    auto transport = std::move(*transport_result);
    result.set_err = transport->set_read_callback(&record_tcp_callback, &result);
    result.wait_result = co_await transport->wait_readable(1s);
    transport->close();
    done->set_value(std::move(result));
    co_return;
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

TEST(HttpTransportTest, TcpFactoryAppliesNoDelayOptions) {
    int server_fd = -1;
    int client_fd = -1;
    ASSERT_TRUE(create_connected_tcp_sockets(server_fd, client_fd));

    TcpNoDelayResult enabled = run_tcp_no_delay_check(server_fd, {.no_delay = fiber::net::TcpOptionMode::Enabled});
    EXPECT_EQ(enabled.error, fiber::common::IoErr::None);
    EXPECT_EQ(enabled.value, 1);
    ::close(client_fd);

    ASSERT_TRUE(create_connected_tcp_sockets(server_fd, client_fd));
    int preset = 1;
    ASSERT_EQ(::setsockopt(server_fd, IPPROTO_TCP, TCP_NODELAY, &preset, sizeof(preset)), 0);
    TcpNoDelayResult unchanged = run_tcp_no_delay_check(server_fd, {});
    EXPECT_EQ(unchanged.error, fiber::common::IoErr::None);
    EXPECT_EQ(unchanged.value, 1);
    ::close(client_fd);

    ASSERT_TRUE(create_connected_tcp_sockets(server_fd, client_fd));
    ASSERT_EQ(::setsockopt(server_fd, IPPROTO_TCP, TCP_NODELAY, &preset, sizeof(preset)), 0);
    TcpNoDelayResult disabled = run_tcp_no_delay_check(server_fd, {.no_delay = fiber::net::TcpOptionMode::Disabled});
    EXPECT_EQ(disabled.error, fiber::common::IoErr::None);
    EXPECT_EQ(disabled.value, 0);
    ::close(client_fd);
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

TEST(HttpTransportTest, TcpPollReadUsesPersistentReadinessCallback) {
    int fds[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);

    fiber::event::EventLoopGroup group(1);
    group.start();
    std::promise<TcpCallbackResult> done_promise;
    auto done_future = done_promise.get_future();
    fiber::async::spawn(group.at(0),
                        [&]() { return exercise_tcp_callback(&group.at(0), fds[0], fds[1], &done_promise); });

    ASSERT_EQ(done_future.wait_for(2s), std::future_status::ready);
    TcpCallbackResult result = done_future.get();
    group.stop();
    group.join();
    ::close(fds[1]);

    EXPECT_EQ(result.initial_poll_err, fiber::common::IoErr::WouldBlock);
    EXPECT_EQ(result.initial_wait_event, fiber::event::IoEvent::Read);
    EXPECT_EQ(result.set_err, fiber::common::IoErr::None);
    EXPECT_TRUE(result.called);
    EXPECT_EQ(result.callback_err, fiber::common::IoErr::None);
    EXPECT_EQ(result.clear_err, fiber::common::IoErr::None);
    EXPECT_EQ(result.callback_poll_err, fiber::common::IoErr::None);
    EXPECT_EQ(result.callback_wait_event, fiber::event::IoEvent::None);
    EXPECT_EQ(result.read, 5U);
    EXPECT_EQ(std::string_view(result.data.data(), result.read), "hello");
}

TEST(HttpTransportTest, TcpReadCallbackConflictsWithWaitAndCloseCancelsIt) {
    int fds[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);

    fiber::event::EventLoopGroup group(1);
    group.start();
    std::promise<TcpBusyResult> done_promise;
    auto done_future = done_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return exercise_tcp_callback_busy(&group.at(0), fds[0], &done_promise); });

    ASSERT_EQ(done_future.wait_for(2s), std::future_status::ready);
    TcpBusyResult result = done_future.get();
    group.stop();
    group.join();
    ::close(fds[1]);

    EXPECT_EQ(result.set_err, fiber::common::IoErr::None);
    ASSERT_FALSE(result.wait_result);
    EXPECT_EQ(result.wait_result.error(), fiber::common::IoErr::Busy);
    EXPECT_EQ(result.callback_calls, 1U);
    EXPECT_EQ(result.callback_err, fiber::common::IoErr::Canceled);
}

} // namespace
