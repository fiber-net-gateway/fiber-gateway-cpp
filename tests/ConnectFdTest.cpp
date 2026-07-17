#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <future>
#include <sys/socket.h>
#include <unistd.h>

#include "async/Sleep.h"
#include "async/Spawn.h"
#include "common/IoError.h"
#include "event/EventLoopGroup.h"
#include "net/detail/ConnectFd.h"

namespace {

using namespace std::chrono_literals;
using fiber::async::DetachedTask;

struct BlockedConnectState {
    int peer_fd = -1;
};

struct BlockedConnectAddress {
    BlockedConnectState *state = nullptr;
};

struct BlockedConnectTraits {
    using Address = BlockedConnectAddress;

    static fiber::common::IoResult<int> create_socket(const Address &address) {
        if (!address.state) {
            return std::unexpected(fiber::common::IoErr::Invalid);
        }

        int fds[2] = {-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) != 0) {
            return std::unexpected(fiber::common::io_err_from_errno(errno));
        }

        int send_buffer_size = 4096;
        (void) ::setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &send_buffer_size, sizeof(send_buffer_size));
        std::array<char, 4096> bytes{};
        for (;;) {
            ssize_t written = ::send(fds[0], bytes.data(), bytes.size(), MSG_NOSIGNAL);
            if (written > 0) {
                continue;
            }
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            }
            int err = written < 0 ? errno : EIO;
            ::close(fds[0]);
            ::close(fds[1]);
            return std::unexpected(fiber::common::io_err_from_errno(err));
        }

        address.state->peer_fd = fds[1];
        return fds[0];
    }

    static fiber::common::IoErr connect_once(int, const Address &) { return fiber::common::IoErr::WouldBlock; }
};

using BlockedConnect = fiber::net::detail::ConnectFd<BlockedConnectTraits>;

void close_peer(BlockedConnectState &state) noexcept {
    if (state.peer_fd < 0) {
        return;
    }
    ::close(state.peer_fd);
    state.peer_fd = -1;
}

DetachedTask run_connect(std::promise<fiber::common::IoErr> *promise, BlockedConnectState *state,
                         std::chrono::milliseconds timeout) {
    auto result = co_await BlockedConnect::connect(fiber::event::EventLoop::current(), {state}, timeout);
    close_peer(*state);
    promise->set_value(result ? fiber::common::IoErr::None : result.error());
    fiber::event::EventLoop::current().stop();
}

DetachedTask drain_peer(BlockedConnectState *state) {
    co_await fiber::async::sleep(10ms);
    std::array<char, 16384> bytes{};
    for (;;) {
        ssize_t read = ::recv(state->peer_fd, bytes.data(), bytes.size(), 0);
        if (read > 0) {
            continue;
        }
        if (read < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
}

DetachedTask run_complete_before_timeout(std::promise<fiber::common::IoErr> *promise, BlockedConnectState *state) {
    fiber::async::spawn([state]() { return drain_peer(state); });
    auto result = co_await BlockedConnect::connect(fiber::event::EventLoop::current(), {state}, 50ms);
    fiber::common::IoErr err = result ? fiber::common::IoErr::None : result.error();
    close_peer(*state);
    co_await fiber::async::sleep(70ms);
    promise->set_value(err);
    fiber::event::EventLoop::current().stop();
}

DetachedTask cancel_connect(BlockedConnect::ConnectAwaiter *connect) {
    co_await fiber::async::sleep(10ms);
    connect->cancel();
}

DetachedTask run_cancel(std::promise<fiber::common::IoErr> *promise, BlockedConnectState *state) {
    auto connect =
            BlockedConnect::connect(fiber::event::EventLoop::current(), {state}, std::chrono::milliseconds::max());
    fiber::async::spawn([&connect]() { return cancel_connect(&connect); });
    auto result = co_await connect;
    close_peer(*state);
    promise->set_value(result ? fiber::common::IoErr::None : result.error());
    fiber::event::EventLoop::current().stop();
}

fiber::common::IoErr await_connect_result(auto &&factory, BlockedConnectState &state) {
    fiber::event::EventLoopGroup group(1);
    std::promise<fiber::common::IoErr> promise;
    auto future = promise.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() { return factory(&promise, &state); });

    if (future.wait_for(2s) != std::future_status::ready) {
        group.stop();
        group.join();
        close_peer(state);
        return fiber::common::IoErr::Unknown;
    }
    fiber::common::IoErr result = future.get();
    group.join();
    close_peer(state);
    return result;
}

TEST(ConnectFdTest, ZeroTimeoutDoesNotWait) {
    BlockedConnectState state;
    EXPECT_EQ(
            await_connect_result(
                    [](auto *promise, auto *connect_state) { return run_connect(promise, connect_state, 0ms); }, state),
            fiber::common::IoErr::TimedOut);
}

TEST(ConnectFdTest, ReturnsTimedOutWhenDeadlineExpires) {
    BlockedConnectState state;
    EXPECT_EQ(await_connect_result(
                      [](auto *promise, auto *connect_state) { return run_connect(promise, connect_state, 20ms); },
                      state),
              fiber::common::IoErr::TimedOut);
}

TEST(ConnectFdTest, CompletionCancelsTimer) {
    BlockedConnectState state;
    EXPECT_EQ(await_connect_result(
                      [](auto *promise, auto *connect_state) {
                          return run_complete_before_timeout(promise, connect_state);
                      },
                      state),
              fiber::common::IoErr::None);
}

TEST(ConnectFdTest, CancelStopsInfiniteWait) {
    BlockedConnectState state;
    EXPECT_EQ(await_connect_result(
                      [](auto *promise, auto *connect_state) { return run_cancel(promise, connect_state); }, state),
              fiber::common::IoErr::Canceled);
}

} // namespace
