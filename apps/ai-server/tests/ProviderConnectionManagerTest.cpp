#include "provider/ProviderConnectionManager.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <string>

#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <async/Sleep.h>
#include <async/Spawn.h>
#include <common/IoError.h>
#include <event/EventLoopGroup.h>
#include <net/SocketAddress.h>
#include <net/TcpListener.h>

namespace fiber::ai_server {
namespace {

using namespace std::chrono_literals;

struct HoldServerState {
    std::atomic_bool stop{false};
};

common::IoResult<std::uint16_t> resolve_port(int fd) noexcept {
    sockaddr_storage bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&bound), &len) != 0) {
        return std::unexpected(common::io_err_from_errno(errno));
    }
    net::SocketAddress local;
    if (!net::SocketAddress::from_sockaddr(reinterpret_cast<sockaddr *>(&bound), len, local)) {
        return std::unexpected(common::IoErr::NotSupported);
    }
    return local.port();
}

async::DetachedTask run_hold_server(event::EventLoop &loop, std::promise<std::uint16_t> &port_promise,
                                    std::promise<common::IoErr> &result_promise,
                                    const std::shared_ptr<HoldServerState> &state) noexcept {
    net::TcpListener listener(loop);
    auto bound = listener.bind(net::SocketAddress(net::IpAddress::loopback_v4(), 0), {});
    if (!bound) {
        port_promise.set_value(0);
        result_promise.set_value(bound.error());
        co_return;
    }

    auto port = resolve_port(listener.fd());
    port_promise.set_value(port ? *port : 0);
    if (!port) {
        listener.close();
        result_promise.set_value(port.error());
        co_return;
    }

    auto accepted = co_await listener.accept();
    if (!accepted) {
        listener.close();
        result_promise.set_value(accepted.error());
        co_return;
    }
    const int client_fd = accepted->release_fd();
    listener.close();

    while (!state->stop.load(std::memory_order_acquire)) {
        co_await async::sleep(1ms);
    }

    (void) ::close(client_fd);
    result_promise.set_value(common::IoErr::None);
}

struct HomeAcquireOutcome {
    http::Http1ClientConnection *connection = nullptr;
    common::IoErr error = common::IoErr::Unknown;
    bool initialized = false;
    bool hit = true;
};

struct BorrowAcquireOutcome {
    common::IoErr error = common::IoErr::Unknown;
    bool hit = false;
    bool same_connection = false;
    bool home_loop_preserved = false;
    bool borrower_loop_preserved = false;
};

TEST(ProviderConnectionManagerTest, StealsIdleConnectionAcrossWorkers) {
    event::EventLoopGroup server_group(1);
    auto server_state = std::make_shared<HoldServerState>();
    std::promise<std::uint16_t> server_port_promise;
    std::promise<common::IoErr> server_result_promise;
    auto server_port_future = server_port_promise.get_future();
    auto server_result_future = server_result_promise.get_future();

    server_group.start();
    async::spawn(server_group.at(0), [&]() {
        return run_hold_server(server_group.at(0), server_port_promise, server_result_promise, server_state);
    });

    ASSERT_EQ(server_port_future.wait_for(10s), std::future_status::ready);
    const std::uint16_t port = server_port_future.get();
    ASSERT_NE(port, 0);

    auto config = std::make_shared<ProviderConfigSnapshot>();
    config->name = "provider";
    config->base_url = "http://127.0.0.1:" + std::to_string(port);
    ProviderProtocol protocol{
            .type = ProviderProtocolType::OpenAiChatCompletions,
            .path = "/v1/chat/completions",
            .model = "test-model",
    };
    ProjectProvider provider{
            .name = "provider",
            .config = std::move(config),
    };
    const ResolvedProviderAttempt attempt{
            .provider = &provider,
            .protocol = &protocol,
    };

    event::EventLoopGroup workers(2);
    ProviderConnectionManager manager(workers);
    std::promise<HomeAcquireOutcome> home_promise;
    std::promise<BorrowAcquireOutcome> borrower_promise;
    std::promise<void> shutdown_promise;
    auto home_future = home_promise.get_future();
    auto borrower_future = borrower_promise.get_future();
    auto shutdown_future = shutdown_promise.get_future();

    workers.start();
    async::spawn(workers.at(0), [&]() -> async::DetachedTask {
        HomeAcquireOutcome outcome;
        outcome.initialized = co_await manager.init();
        if (outcome.initialized) {
            auto acquired = co_await manager.acquire(attempt, 1s);
            if (acquired) {
                outcome.connection = acquired->connection;
                outcome.error = common::IoErr::None;
                outcome.hit = acquired->lease.hit();
                acquired->lease.reset();
            } else {
                outcome.error = acquired.error().io_error;
            }
        }
        home_promise.set_value(outcome);
    });

    ASSERT_EQ(home_future.wait_for(10s), std::future_status::ready);
    const HomeAcquireOutcome home = home_future.get();
    ASSERT_TRUE(home.initialized);
    ASSERT_EQ(home.error, common::IoErr::None);
    ASSERT_NE(home.connection, nullptr);
    EXPECT_FALSE(home.hit);

    async::spawn(workers.at(1), [&]() -> async::DetachedTask {
        BorrowAcquireOutcome outcome;
        auto acquired = co_await manager.acquire(attempt, 1s);
        if (acquired) {
            outcome.error = common::IoErr::None;
            outcome.hit = acquired->lease.hit();
            outcome.same_connection = acquired->connection == home.connection;
            outcome.home_loop_preserved = &acquired->connection->loop() == &workers.at(0);
            outcome.borrower_loop_preserved = &event::EventLoop::current() == &workers.at(1);
            acquired->lease.reset();
        } else {
            outcome.error = acquired.error().io_error;
        }
        borrower_promise.set_value(outcome);
    });

    ASSERT_EQ(borrower_future.wait_for(10s), std::future_status::ready);
    const BorrowAcquireOutcome borrower = borrower_future.get();
    EXPECT_EQ(borrower.error, common::IoErr::None);
    EXPECT_TRUE(borrower.hit);
    EXPECT_TRUE(borrower.same_connection);
    EXPECT_TRUE(borrower.home_loop_preserved);
    EXPECT_TRUE(borrower.borrower_loop_preserved);

    async::spawn(workers.at(0), [&]() -> async::DetachedTask {
        co_await manager.shutdown();
        shutdown_promise.set_value();
    });
    ASSERT_EQ(shutdown_future.wait_for(10s), std::future_status::ready);

    workers.stop();
    workers.join();
    server_state->stop.store(true, std::memory_order_release);
    ASSERT_EQ(server_result_future.wait_for(10s), std::future_status::ready);
    EXPECT_EQ(server_result_future.get(), common::IoErr::None);
    server_group.stop();
    server_group.join();
}

} // namespace
} // namespace fiber::ai_server
