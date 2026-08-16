#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <future>
#include <span>
#include <sys/socket.h>
#include <unistd.h>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/async/WhenAny.h>
#include <fiber/common/IoError.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/net/TcpConnector.h>
#include <fiber/net/TcpListener.h>
#include <fiber/net/detail/HappyEyeballsConnectFd.h>

namespace {

using namespace std::chrono_literals;
using fiber::async::DetachedTask;

enum class FakeBehavior : std::uint8_t {
    Pending,
    ImmediateSuccess,
    ImmediateFailure,
};

struct FakeConnectState {
    FakeConnectState() {
        peer_fds.fill(-1);
        behaviors.fill(FakeBehavior::Pending);
        errors.fill(fiber::common::IoErr::ConnRefused);
    }

    std::array<FakeBehavior, fiber::net::kHappyEyeballsMaxAddresses> behaviors{};
    std::array<fiber::common::IoErr, fiber::net::kHappyEyeballsMaxAddresses> errors{};
    std::array<int, fiber::net::kHappyEyeballsMaxAddresses> peer_fds{};
    std::array<std::uint8_t, fiber::net::kHappyEyeballsMaxAddresses> launch_order{};
    std::array<std::chrono::steady_clock::time_point, fiber::net::kHappyEyeballsMaxAddresses> launch_times{};
    std::promise<void> *first_launch = nullptr;
    std::uint8_t launch_count = 0;
};

struct FakeAddress {
    FakeConnectState *state = nullptr;
    fiber::net::IpFamily ip_family = fiber::net::IpFamily::V4;
    std::uint8_t id = 0;

    [[nodiscard]] fiber::net::IpFamily family() const noexcept { return ip_family; }
};

bool fill_send_buffer(int fd) noexcept {
    int send_buffer_size = 4096;
    (void) ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &send_buffer_size, sizeof(send_buffer_size));
    std::array<char, 4096> bytes{};
    for (;;) {
        ssize_t written = ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
        if (written > 0) {
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        return written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
    }
}

struct FakeConnectTraits {
    using Address = FakeAddress;

    static fiber::common::IoResult<int> create_socket(const Address &address) noexcept {
        if (address.state == nullptr || address.id >= address.state->behaviors.size()) {
            return std::unexpected(fiber::common::IoErr::Invalid);
        }

        int fds[2] = {-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) != 0) {
            return std::unexpected(fiber::common::io_err_from_errno(errno));
        }
        if (address.state->behaviors[address.id] == FakeBehavior::Pending && !fill_send_buffer(fds[0])) {
            ::close(fds[0]);
            ::close(fds[1]);
            return std::unexpected(fiber::common::IoErr::Unknown);
        }

        address.state->peer_fds[address.id] = fds[1];
        const std::uint8_t launch_index = address.state->launch_count++;
        address.state->launch_order[launch_index] = address.id;
        address.state->launch_times[launch_index] = fiber::event::EventLoop::current().now();
        if (address.state->launch_count == 1 && address.state->first_launch != nullptr) {
            address.state->first_launch->set_value();
        }
        return fds[0];
    }

    static fiber::common::IoErr connect_once(int, const Address &address) noexcept {
        switch (address.state->behaviors[address.id]) {
            case FakeBehavior::Pending:
                return fiber::common::IoErr::WouldBlock;
            case FakeBehavior::ImmediateSuccess:
                return fiber::common::IoErr::None;
            case FakeBehavior::ImmediateFailure:
                return address.state->errors[address.id];
        }
        return fiber::common::IoErr::Unknown;
    }
};

using FakeConnector = fiber::net::detail::HappyEyeballsConnectFd<FakeConnectTraits>;

bool peer_closed(int fd) noexcept {
    if (fd < 0) {
        return false;
    }
    std::array<char, 4096> bytes{};
    for (;;) {
        ssize_t received = ::recv(fd, bytes.data(), bytes.size(), 0);
        if (received > 0) {
            continue;
        }
        if (received == 0) {
            return true;
        }
        if (errno == EINTR) {
            continue;
        }
        return false;
    }
}

void close_peers(FakeConnectState &state) noexcept {
    for (int &fd: state.peer_fds) {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }
}

struct PolicyOutcome {
    fiber::net::HappyEyeballsConnectError error{};
    std::array<std::uint8_t, 4> launch_order{};
    std::array<std::chrono::steady_clock::time_point, 4> launch_times{};
    std::uint8_t launch_count = 0;
};

DetachedTask run_policy_case(FakeConnectState *state, fiber::net::HappyEyeballsAddressPolicy policy,
                             std::promise<PolicyOutcome> *promise) {
    std::array<FakeAddress, 4> addresses{{
            {state, fiber::net::IpFamily::V4, 0},
            {state, fiber::net::IpFamily::V6, 1},
            {state, fiber::net::IpFamily::V6, 2},
            {state, fiber::net::IpFamily::V4, 3},
    }};
    fiber::net::HappyEyeballsOptions options;
    options.address_policy = policy;
    options.connection_attempt_delay = 10ms;
    options.total_timeout = 1s;

    auto result = co_await FakeConnector::connect(fiber::event::EventLoop::current(), addresses, options);
    PolicyOutcome outcome;
    if (!result) {
        outcome.error = result.error();
    }
    outcome.launch_count = state->launch_count;
    std::copy_n(state->launch_order.begin(), outcome.launch_order.size(), outcome.launch_order.begin());
    std::copy_n(state->launch_times.begin(), outcome.launch_times.size(), outcome.launch_times.begin());
    close_peers(*state);
    promise->set_value(outcome);
    fiber::event::EventLoop::current().stop();
}

PolicyOutcome execute_policy_case(fiber::net::HappyEyeballsAddressPolicy policy) {
    FakeConnectState state;
    state.behaviors.fill(FakeBehavior::ImmediateFailure);
    state.errors[0] = fiber::common::IoErr::AddrNotAvailable;
    state.errors[1] = fiber::common::IoErr::ConnRefused;
    state.errors[2] = fiber::common::IoErr::Permission;
    state.errors[3] = fiber::common::IoErr::NotConnected;

    fiber::event::EventLoopGroup group(1);
    std::promise<PolicyOutcome> promise;
    auto future = promise.get_future();
    group.start();
    fiber::async::spawn(group.at(0), [&]() { return run_policy_case(&state, policy, &promise); });
    PolicyOutcome outcome = future.get();
    group.join();
    return outcome;
}

TEST(TcpConnectorTest, InterleavesV6FirstAndSummarizesFailuresInCandidateOrder) {
    PolicyOutcome outcome = execute_policy_case(fiber::net::HappyEyeballsAddressPolicy::V6First);
    const std::array<std::uint8_t, 4> expected_order{1, 0, 2, 3};
    EXPECT_EQ(outcome.launch_count, 4);
    EXPECT_EQ(outcome.launch_order, expected_order);
    EXPECT_EQ(outcome.error.code, fiber::common::IoErr::NotConnected);
    EXPECT_EQ(outcome.error.candidate_count, 4);
    EXPECT_EQ(outcome.error.attempted_count, 4);
    EXPECT_EQ(outcome.error.attempted_mask, 0x0f);
    EXPECT_EQ(outcome.error.failed_mask, 0x0f);
    EXPECT_EQ(outcome.error.input_indices[0], 1);
    EXPECT_EQ(outcome.error.input_indices[1], 0);
    EXPECT_EQ(outcome.error.input_indices[2], 2);
    EXPECT_EQ(outcome.error.input_indices[3], 3);
    EXPECT_EQ(outcome.error.attempt_errors[0], fiber::common::IoErr::ConnRefused);
    EXPECT_EQ(outcome.error.attempt_errors[1], fiber::common::IoErr::AddrNotAvailable);
    EXPECT_EQ(outcome.error.attempt_errors[2], fiber::common::IoErr::Permission);
    EXPECT_EQ(outcome.error.attempt_errors[3], fiber::common::IoErr::NotConnected);
    for (std::size_t i = 1; i < outcome.launch_times.size(); ++i) {
        EXPECT_GE(outcome.launch_times[i] - outcome.launch_times[i - 1],
                  fiber::net::kHappyEyeballsMinimumConnectionAttemptDelay);
    }
}

TEST(TcpConnectorTest, InterleavesV4FirstDeterministically) {
    PolicyOutcome outcome = execute_policy_case(fiber::net::HappyEyeballsAddressPolicy::V4First);
    const std::array<std::uint8_t, 4> expected_order{0, 1, 3, 2};
    EXPECT_EQ(outcome.launch_count, 4);
    EXPECT_EQ(outcome.launch_order, expected_order);
    EXPECT_EQ(outcome.error.code, fiber::common::IoErr::Permission);
    EXPECT_EQ(outcome.error.input_indices[0], 0);
    EXPECT_EQ(outcome.error.input_indices[1], 1);
    EXPECT_EQ(outcome.error.input_indices[2], 3);
    EXPECT_EQ(outcome.error.input_indices[3], 2);
}

struct ConnectorOutcome {
    fiber::common::IoErr error = fiber::common::IoErr::Unknown;
    fiber::net::IpFamily winner_family = fiber::net::IpFamily::V6;
    std::uint8_t launch_count = 0;
    bool first_peer_closed = false;
    bool second_peer_closed = false;
    std::chrono::milliseconds elapsed{};
};

DetachedTask cancel_after(FakeConnector::ConnectAwaiter *connect) {
    co_await fiber::async::sleep(45ms);
    connect->cancel();
}

DetachedTask run_concurrency_cap(FakeConnectState *state, std::promise<ConnectorOutcome> *promise) {
    std::array<FakeAddress, 4> addresses{{
            {state, fiber::net::IpFamily::V6, 0},
            {state, fiber::net::IpFamily::V4, 1},
            {state, fiber::net::IpFamily::V6, 2},
            {state, fiber::net::IpFamily::V4, 3},
    }};
    fiber::net::HappyEyeballsOptions options;
    options.connection_attempt_delay = 10ms;
    options.max_concurrent_attempts = 2;

    auto connect = FakeConnector::connect(fiber::event::EventLoop::current(), addresses, options);
    fiber::async::spawn([&connect]() { return cancel_after(&connect); });
    auto result = co_await connect;

    ConnectorOutcome outcome;
    outcome.error = result ? fiber::common::IoErr::None : result.error().code;
    outcome.launch_count = state->launch_count;
    outcome.first_peer_closed = peer_closed(state->peer_fds[0]);
    outcome.second_peer_closed = peer_closed(state->peer_fds[1]);
    close_peers(*state);
    promise->set_value(outcome);
    fiber::event::EventLoop::current().stop();
}

TEST(TcpConnectorTest, EnforcesConcurrencyCapAndCallerCancellationClosesAttempts) {
    FakeConnectState state;
    fiber::event::EventLoopGroup group(1);
    std::promise<ConnectorOutcome> promise;
    auto future = promise.get_future();
    group.start();
    fiber::async::spawn(group.at(0), [&]() { return run_concurrency_cap(&state, &promise); });
    ConnectorOutcome outcome = future.get();
    group.join();

    EXPECT_EQ(outcome.error, fiber::common::IoErr::Canceled);
    EXPECT_EQ(outcome.launch_count, 2);
    EXPECT_TRUE(outcome.first_peer_closed);
    EXPECT_TRUE(outcome.second_peer_closed);
}

DetachedTask run_staggered_winner(FakeConnectState *state, std::promise<ConnectorOutcome> *promise) {
    std::array<FakeAddress, 2> addresses{{
            {state, fiber::net::IpFamily::V6, 0},
            {state, fiber::net::IpFamily::V4, 1},
    }};
    fiber::net::HappyEyeballsOptions options;
    options.connection_attempt_delay = 20ms;
    options.total_timeout = 1s;
    const auto started = fiber::event::EventLoop::current().now();
    auto result = co_await FakeConnector::connect(fiber::event::EventLoop::current(), addresses, options);

    ConnectorOutcome outcome;
    outcome.elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(fiber::event::EventLoop::current().now() - started);
    outcome.error = result ? fiber::common::IoErr::None : result.error().code;
    outcome.launch_count = state->launch_count;
    if (result) {
        outcome.winner_family = result->peer().family();
        const int winner_fd = result->release_fd();
        ::close(winner_fd);
    }
    outcome.first_peer_closed = peer_closed(state->peer_fds[0]);
    close_peers(*state);
    promise->set_value(outcome);
    fiber::event::EventLoop::current().stop();
}

TEST(TcpConnectorTest, FirstSuccessWinsAndClosesPendingLoserBeforeReturning) {
    FakeConnectState state;
    state.behaviors[1] = FakeBehavior::ImmediateSuccess;
    fiber::event::EventLoopGroup group(1);
    std::promise<ConnectorOutcome> promise;
    auto future = promise.get_future();
    group.start();
    fiber::async::spawn(group.at(0), [&]() { return run_staggered_winner(&state, &promise); });
    ConnectorOutcome outcome = future.get();
    group.join();

    EXPECT_EQ(outcome.error, fiber::common::IoErr::None);
    EXPECT_EQ(outcome.winner_family, fiber::net::IpFamily::V4);
    EXPECT_EQ(outcome.launch_count, 2);
    EXPECT_GE(outcome.elapsed, 10ms);
    EXPECT_LT(outcome.elapsed, 500ms);
    EXPECT_TRUE(outcome.first_peer_closed);
}

void drain_peer(int fd) noexcept {
    std::array<char, 4096> bytes{};
    for (;;) {
        ssize_t received = ::recv(fd, bytes.data(), bytes.size(), 0);
        if (received > 0 || (received < 0 && errno == EINTR)) {
            continue;
        }
        return;
    }
}

DetachedTask make_attempts_writable(FakeConnectState *state) {
    co_await fiber::async::sleep(40ms);
    drain_peer(state->peer_fds[0]);
    drain_peer(state->peer_fds[1]);
}

DetachedTask run_simultaneous_ready_attempts(FakeConnectState *state, std::promise<ConnectorOutcome> *promise) {
    std::array<FakeAddress, 2> addresses{{
            {state, fiber::net::IpFamily::V6, 0},
            {state, fiber::net::IpFamily::V4, 1},
    }};
    fiber::net::HappyEyeballsOptions options;
    options.connection_attempt_delay = 10ms;
    options.total_timeout = 1s;
    fiber::async::spawn([state]() { return make_attempts_writable(state); });

    auto result = co_await FakeConnector::connect(fiber::event::EventLoop::current(), addresses, options);
    ConnectorOutcome outcome;
    outcome.error = result ? fiber::common::IoErr::None : result.error().code;
    outcome.launch_count = state->launch_count;
    if (result) {
        const int winner_fd = result->release_fd();
        ::close(winner_fd);
    }
    outcome.first_peer_closed = peer_closed(state->peer_fds[0]);
    outcome.second_peer_closed = peer_closed(state->peer_fds[1]);
    close_peers(*state);
    promise->set_value(outcome);
    fiber::event::EventLoop::current().stop();
}

TEST(TcpConnectorTest, SimultaneousReadyAttemptsKeepLoserCallbacksAliveThroughPollDispatch) {
    FakeConnectState state;
    fiber::event::EventLoopGroup group(1);
    std::promise<ConnectorOutcome> promise;
    auto future = promise.get_future();
    group.start();
    fiber::async::spawn(group.at(0), [&]() { return run_simultaneous_ready_attempts(&state, &promise); });
    ConnectorOutcome outcome = future.get();
    group.join();

    EXPECT_EQ(outcome.error, fiber::common::IoErr::None);
    EXPECT_EQ(outcome.launch_count, 2);
    EXPECT_TRUE(outcome.first_peer_closed);
    EXPECT_TRUE(outcome.second_peer_closed);
}

DetachedTask run_deadline(FakeConnectState *state, std::promise<ConnectorOutcome> *promise) {
    std::array<FakeAddress, 3> addresses{{
            {state, fiber::net::IpFamily::V6, 0},
            {state, fiber::net::IpFamily::V4, 1},
            {state, fiber::net::IpFamily::V6, 2},
    }};
    fiber::net::HappyEyeballsOptions options;
    options.connection_attempt_delay = 10ms;
    options.max_concurrent_attempts = 2;
    options.total_timeout = 35ms;
    const auto started = fiber::event::EventLoop::current().now();
    auto result = co_await FakeConnector::connect(fiber::event::EventLoop::current(), addresses, options);

    ConnectorOutcome outcome;
    outcome.elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(fiber::event::EventLoop::current().now() - started);
    outcome.error = result ? fiber::common::IoErr::None : result.error().code;
    outcome.launch_count = state->launch_count;
    outcome.first_peer_closed = peer_closed(state->peer_fds[0]);
    outcome.second_peer_closed = peer_closed(state->peer_fds[1]);
    close_peers(*state);
    promise->set_value(outcome);
    fiber::event::EventLoop::current().stop();
}

TEST(TcpConnectorTest, OneTotalDeadlineCoversAllAttempts) {
    FakeConnectState state;
    fiber::event::EventLoopGroup group(1);
    std::promise<ConnectorOutcome> promise;
    auto future = promise.get_future();
    group.start();
    fiber::async::spawn(group.at(0), [&]() { return run_deadline(&state, &promise); });
    ConnectorOutcome outcome = future.get();
    group.join();

    EXPECT_EQ(outcome.error, fiber::common::IoErr::TimedOut);
    EXPECT_EQ(outcome.launch_count, 2);
    EXPECT_GE(outcome.elapsed, 25ms);
    EXPECT_LT(outcome.elapsed, 500ms);
    EXPECT_TRUE(outcome.first_peer_closed);
    EXPECT_TRUE(outcome.second_peer_closed);
}

DetachedTask run_zero_deadline(FakeConnectState *state, std::promise<ConnectorOutcome> *promise) {
    std::array<FakeAddress, 1> addresses{{{state, fiber::net::IpFamily::V6, 0}}};
    fiber::net::HappyEyeballsOptions options;
    options.connection_attempt_delay = 10ms;
    options.total_timeout = 0ms;
    auto result = co_await FakeConnector::connect(fiber::event::EventLoop::current(), addresses, options);

    ConnectorOutcome outcome;
    outcome.error = result ? fiber::common::IoErr::None : result.error().code;
    outcome.launch_count = state->launch_count;
    outcome.first_peer_closed = peer_closed(state->peer_fds[0]);
    close_peers(*state);
    promise->set_value(outcome);
    fiber::event::EventLoop::current().stop();
}

TEST(TcpConnectorTest, ZeroDeadlineMakesOneImmediateAttemptWithoutWaiting) {
    FakeConnectState state;
    fiber::event::EventLoopGroup group(1);
    std::promise<ConnectorOutcome> promise;
    auto future = promise.get_future();
    group.start();
    fiber::async::spawn(group.at(0), [&]() { return run_zero_deadline(&state, &promise); });
    ConnectorOutcome outcome = future.get();
    group.join();

    EXPECT_EQ(outcome.error, fiber::common::IoErr::TimedOut);
    EXPECT_EQ(outcome.launch_count, 1);
    EXPECT_TRUE(outcome.first_peer_closed);
}

DetachedTask run_awaiter_destruction(FakeConnectState *state, std::promise<ConnectorOutcome> *promise) {
    std::array<FakeAddress, 1> addresses{{{state, fiber::net::IpFamily::V6, 0}}};
    fiber::net::HappyEyeballsOptions options;
    options.connection_attempt_delay = 10ms;

    auto result = co_await fiber::async::when_any(
            [&]() { return FakeConnector::connect(fiber::event::EventLoop::current(), addresses, options); },
            []() { return fiber::async::sleep(20ms); });

    ConnectorOutcome outcome;
    outcome.error = result.index() == 1 ? fiber::common::IoErr::Canceled : fiber::common::IoErr::Unknown;
    outcome.launch_count = state->launch_count;
    outcome.first_peer_closed = peer_closed(state->peer_fds[0]);
    close_peers(*state);
    promise->set_value(outcome);
    fiber::event::EventLoop::current().stop();
}

TEST(TcpConnectorTest, AwaiterDestructionClosesPendingAttempts) {
    FakeConnectState state;
    fiber::event::EventLoopGroup group(1);
    std::promise<ConnectorOutcome> promise;
    auto future = promise.get_future();
    group.start();
    fiber::async::spawn(group.at(0), [&]() { return run_awaiter_destruction(&state, &promise); });
    ConnectorOutcome outcome = future.get();
    group.join();

    EXPECT_EQ(outcome.error, fiber::common::IoErr::Canceled);
    EXPECT_EQ(outcome.launch_count, 1);
    EXPECT_TRUE(outcome.first_peer_closed);
}

DetachedTask run_loop_shutdown(FakeConnectState *state, std::promise<ConnectorOutcome> *promise) {
    std::array<FakeAddress, 1> addresses{{{state, fiber::net::IpFamily::V6, 0}}};
    fiber::net::HappyEyeballsOptions options;
    options.connection_attempt_delay = 10ms;
    auto result = co_await FakeConnector::connect(fiber::event::EventLoop::current(), addresses, options);

    ConnectorOutcome outcome;
    outcome.error = result ? fiber::common::IoErr::None : result.error().code;
    outcome.launch_count = state->launch_count;
    outcome.first_peer_closed = peer_closed(state->peer_fds[0]);
    close_peers(*state);
    promise->set_value(outcome);
}

TEST(TcpConnectorTest, EventLoopShutdownCancelsAndClosesPendingAttempts) {
    FakeConnectState state;
    std::promise<void> started_promise;
    auto started_future = started_promise.get_future();
    state.first_launch = &started_promise;

    fiber::event::EventLoopGroup group(1);
    std::promise<ConnectorOutcome> promise;
    auto future = promise.get_future();
    group.start();
    fiber::async::spawn(group.at(0), [&]() { return run_loop_shutdown(&state, &promise); });
    ASSERT_EQ(started_future.wait_for(1s), std::future_status::ready);
    group.stop();
    ASSERT_EQ(future.wait_for(1s), std::future_status::ready);
    ConnectorOutcome outcome = future.get();
    group.join();

    EXPECT_EQ(outcome.error, fiber::common::IoErr::Canceled);
    EXPECT_EQ(outcome.launch_count, 1);
    EXPECT_TRUE(outcome.first_peer_closed);
}

fiber::common::IoResult<std::uint16_t> bound_port(int fd) noexcept {
    sockaddr_storage storage{};
    socklen_t length = sizeof(storage);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&storage), &length) != 0) {
        return std::unexpected(fiber::common::io_err_from_errno(errno));
    }
    fiber::net::SocketAddress address;
    if (!fiber::net::SocketAddress::from_sockaddr(reinterpret_cast<sockaddr *>(&storage), length, address)) {
        return std::unexpected(fiber::common::IoErr::NotSupported);
    }
    return address.port();
}

struct LoopbackOutcome {
    fiber::common::IoErr error = fiber::common::IoErr::Unknown;
    fiber::net::IpFamily winner_family = fiber::net::IpFamily::V6;
    std::chrono::milliseconds elapsed{};
    bool ipv6_supported = true;
};

DetachedTask run_loopback_fallback(std::promise<LoopbackOutcome> *promise) {
    fiber::event::EventLoop &loop = fiber::event::EventLoop::current();
    fiber::net::TcpListener listener(loop);
    auto bind_result = listener.bind(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 0), {});
    auto port_result = bind_result ? bound_port(listener.fd())
                                   : fiber::common::IoResult<std::uint16_t>(std::unexpected(bind_result.error()));
    if (!port_result) {
        promise->set_value({.error = port_result.error()});
        loop.stop();
        co_return;
    }

    int v6_guard = ::socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    fiber::net::SocketAddress v6_address(fiber::net::IpAddress::loopback_v6(), *port_result);
    sockaddr_storage v6_storage{};
    socklen_t v6_length = 0;
    if (v6_guard < 0 || !v6_address.to_sockaddr(v6_storage, v6_length) ||
        ::bind(v6_guard, reinterpret_cast<sockaddr *>(&v6_storage), v6_length) != 0) {
        if (v6_guard >= 0) {
            ::close(v6_guard);
        }
        listener.close();
        promise->set_value({.error = fiber::common::IoErr::NotSupported, .ipv6_supported = false});
        loop.stop();
        co_return;
    }

    std::array<fiber::net::SocketAddress, 2> addresses{{
            v6_address,
            fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), *port_result),
    }};
    fiber::net::HappyEyeballsOptions options;
    options.total_timeout = 2s;
    options.connection_attempt_delay = 250ms;
    const auto started = loop.now();
    auto connect_result = co_await fiber::net::TcpConnector::connect(loop, addresses, options);

    LoopbackOutcome outcome;
    outcome.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(loop.now() - started);
    outcome.error = connect_result ? fiber::common::IoErr::None : connect_result.error().code;
    if (connect_result) {
        outcome.winner_family = connect_result->peer().family();
        auto accepted = co_await listener.accept();
        (void) accepted;
        const int client_fd = connect_result->release_fd();
        ::close(client_fd);
    }
    ::close(v6_guard);
    listener.close();
    promise->set_value(outcome);
    loop.stop();
}

TEST(TcpConnectorTest, LoopbackV6FirstFallsBackToV4BeforeTotalTimeout) {
    fiber::event::EventLoopGroup group(1);
    std::promise<LoopbackOutcome> promise;
    auto future = promise.get_future();
    group.start();
    fiber::async::spawn(group.at(0), [&]() { return run_loopback_fallback(&promise); });
    LoopbackOutcome outcome = future.get();
    group.join();

    if (!outcome.ipv6_supported) {
        GTEST_SKIP() << "IPv6 loopback is unavailable";
    }
    EXPECT_EQ(outcome.error, fiber::common::IoErr::None);
    EXPECT_EQ(outcome.winner_family, fiber::net::IpFamily::V4);
    EXPECT_LT(outcome.elapsed, 1s);
}

} // namespace
