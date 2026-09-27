#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <sys/socket.h>
#include <unistd.h>

#include <fiber/async/Awaitable.h>
#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/async/Task.h>
#include <fiber/async/TaskSelect.h>
#include <fiber/async/Timeout.h>
#include <fiber/async/Watch.h>
#include <fiber/common/IoError.h>
#include <fiber/event/EventLoop.h>
#include <fiber/net/detail/ConnectFd.h>
#include <fiber/net/detail/RWFd.h>

namespace {

using namespace std::chrono_literals;
using DetachedTask = fiber::async::DetachedTask;

// A socketpair peer whose send buffer is pre-filled, so a non-blocking
// connect() on the other end stays in WouldBlock (same trick as
// ConnectFdTest's BlockedConnectTraits).
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

// Concept membership pins the contracts these tests stand on:
// - the fd-family wait awaiters have no completed(), so when_any cannot take
//   them directly; their destruction path is timeout_for, exactly how
//   HttpTransport wraps wait_readable/wait_writable;
// - ConnectAwaiter cancels with a resume (CancellableAwaiter, external active
//   cancellation) but is not Selectable for the same reason;
// - Watch::NextAwaiter's queue retraction is destructor-only and private, so
//   it deliberately does NOT satisfy CancellableAwaiter.
static_assert(!fiber::async::SelectableAwaiter<fiber::net::detail::RWFd::WaitReadableAwaiter>);
static_assert(!fiber::async::SelectableAwaiter<BlockedConnect::ConnectAwaiter>);
static_assert(fiber::async::CancellableAwaiter<BlockedConnect::ConnectAwaiter>);
static_assert(!fiber::async::CancellableAwaiter<fiber::async::Watch<int>::Subscriber::NextAwaiter>);

class DestructionFlag {
public:
    explicit DestructionFlag(bool *destroyed) noexcept : destroyed_(destroyed) {}

    DestructionFlag(const DestructionFlag &) = delete;
    DestructionFlag &operator=(const DestructionFlag &) = delete;

    ~DestructionFlag() { *destroyed_ = true; }

private:
    bool *destroyed_ = nullptr;
};

fiber::async::Task<void> task_parked_on_sleep(bool *frame_destroyed) {
    DestructionFlag flag(frame_destroyed);
    co_await fiber::async::sleep(120ms);
}

fiber::async::Task<fiber::common::IoErr> task_parked_on_connect(BlockedConnectState *state, bool *frame_destroyed) {
    DestructionFlag flag(frame_destroyed);
    auto result = co_await BlockedConnect::connect(fiber::event::EventLoop::current(), {state},
                                                   std::chrono::milliseconds::max());
    co_return result ? fiber::common::IoErr::None : result.error();
}

// timeout_for over Task::select(): the timed-out task's whole frame must be
// destroyed while it is parked on sleep, retracting the sleep timer. The
// settle window outlives the parked sleep's own deadline (120ms), so a timer
// left armed by a broken destructor fires into freed memory here -- ASan
// makes it a hard failure, a plain build usually crashes.
TEST(AwaiterDestructionTest, TimeoutDestroysTaskParkedOnSleep) {
    fiber::event::EventLoop loop;
    bool frame_destroyed = false;
    fiber::common::IoErr observed = fiber::common::IoErr::Unknown;

    fiber::async::spawn(loop, [&]() -> DetachedTask {
        auto result = co_await fiber::async::timeout_for(
                [&frame_destroyed]() { return task_parked_on_sleep(&frame_destroyed).select(); }, 50ms);
        observed = result ? fiber::common::IoErr::None : result.error();
        co_await fiber::async::sleep(200ms);
        loop.stop();
        co_return;
    });

    loop.run();
    EXPECT_EQ(observed, fiber::common::IoErr::TimedOut);
    EXPECT_TRUE(frame_destroyed);
}

// timeout_for destroying a suspended fd-family awaiter: RWFd::WaitAwaiter is
// not Selectable (no completed()), so timeout, not when_any, is its
// destruction path -- exactly how HttpTransport wraps wait_readable. The
// timed-out inner awaiter is torn down mid-suspend with its own 500ms timer
// armed: destruction must retract both the timer and the fd subscription
// without resuming, the peer's late byte must land with no waiter installed,
// and the fd must stay usable for a later wait.
TEST(AwaiterDestructionTest, TimeoutDestroysSuspendedFdWaitAndRetractsTimer) {
    int fds[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);

    fiber::event::EventLoop loop;
    fiber::net::detail::RWFd rwfd(loop, fds[0]);
    fiber::common::IoErr observed = fiber::common::IoErr::Unknown;
    fiber::common::IoErr second_wait = fiber::common::IoErr::Unknown;

    fiber::async::spawn(loop, [peer = fds[1]]() -> DetachedTask {
        co_await fiber::async::sleep(40ms);
        const char byte = 'x';
        (void) ::send(peer, &byte, 1, MSG_NOSIGNAL);
    });

    fiber::async::spawn(loop, [&]() -> DetachedTask {
        auto result = co_await fiber::async::timeout_for([&rwfd]() { return rwfd.wait_readable(500ms); }, 20ms);
        observed = result ? fiber::common::IoErr::None : result.error();
        // Outlive the destroyed inner awaiter's own 500ms timer (a timer left
        // armed by a broken destructor fires into freed memory here; ASan
        // makes it a hard failure) and the late byte at ~40ms.
        co_await fiber::async::sleep(650ms);
        auto again = co_await rwfd.wait_readable(1s);
        second_wait = again ? fiber::common::IoErr::None : again.error();
        rwfd.close();
        (void) ::close(fds[1]);
        loop.stop();
        co_return;
    });

    loop.run();
    EXPECT_EQ(observed, fiber::common::IoErr::TimedOut);
    EXPECT_EQ(second_wait, fiber::common::IoErr::None);
}

// timeout_for destroying a task parked on a leaf fd awaiter (ConnectAwaiter,
// not Selectable): frame destruction must run ~ConnectAwaiter -> cancel_wait,
// removing the connect socket from the poller and closing it. The poller
// registration count is the leak oracle here.
TEST(AwaiterDestructionTest, TimeoutDestroysTaskParkedOnConnectAndCleansPoller) {
    fiber::event::EventLoop loop;
    BlockedConnectState state;
    bool frame_destroyed = false;
    fiber::common::IoErr observed = fiber::common::IoErr::Unknown;
    bool poller_back_to_baseline = false;

    fiber::async::spawn(loop, [&]() -> DetachedTask {
        const std::size_t baseline = loop.poller().size();
        auto result = co_await fiber::async::timeout_for(
                [&state, &frame_destroyed]() { return task_parked_on_connect(&state, &frame_destroyed).select(); },
                50ms);
        poller_back_to_baseline = loop.poller().size() == baseline;
        observed = result ? fiber::common::IoErr::None : result.error();
        co_await fiber::async::sleep(30ms);
        loop.stop();
        co_return;
    });

    loop.run();
    close_peer(state);
    EXPECT_EQ(observed, fiber::common::IoErr::TimedOut);
    EXPECT_TRUE(poller_back_to_baseline);
    EXPECT_TRUE(frame_destroyed);
}

} // namespace
