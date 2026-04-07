#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <vector>

#include <unistd.h>

#include "async/Sleep.h"
#include "async/Spawn.h"
#include "common/IoError.h"
#include "event/EventLoopGroup.h"
#include "http/Http1ConnectionGroupKey.h"
#include "http/StealableHttp1ConnectionPoolSet.h"
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

fiber::http::Http1ClientConnectionOptions client_options(std::uint16_t port) {
    fiber::http::Http1ClientConnectionOptions options;
    options.peer_addr = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port);
    options.tls.enabled = false;
    return options;
}

struct HoldServerState {
    std::atomic_bool stop{false};
};

DetachedTask run_hold_server(fiber::event::EventLoop *loop,
                             std::size_t accept_count,
                             std::promise<std::uint16_t> *port_promise,
                             std::promise<fiber::common::IoErr> *result_promise,
                             std::shared_ptr<HoldServerState> state) {
    fiber::net::TcpListener listener(*loop);
    fiber::net::ListenOptions options{};
    auto bind_result = listener.bind(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 0), options);
    if (!bind_result) {
        port_promise->set_value(0);
        result_promise->set_value(bind_result.error());
        co_return;
    }

    auto port_result = resolve_port(listener.fd());
    port_promise->set_value(port_result ? *port_result : 0);
    if (!port_result) {
        listener.close();
        result_promise->set_value(port_result.error());
        co_return;
    }

    std::vector<int> accepted_fds;
    accepted_fds.reserve(accept_count);
    for (std::size_t i = 0; i < accept_count; ++i) {
        auto accept_result = co_await listener.accept();
        if (!accept_result) {
            for (int fd : accepted_fds) {
                ::close(fd);
            }
            listener.close();
            result_promise->set_value(accept_result.error());
            co_return;
        }
        accepted_fds.push_back(accept_result->release_fd());
    }

    listener.close();
    while (!state->stop.load(std::memory_order_acquire)) {
        co_await fiber::async::sleep(1ms);
    }

    for (int fd : accepted_fds) {
        ::close(fd);
    }
    result_promise->set_value(fiber::common::IoErr::None);
}

fiber::async::Task<fiber::common::IoResult<fiber::http::Http1ClientConnection *>>
ensure_connected(fiber::http::StealableHttp1ConnectionPoolSet::Lease &lease, std::uint16_t port) {
    if (!lease.valid()) {
        co_return std::unexpected(fiber::common::IoErr::NoMem);
    }
    if (!lease.has_connection()) {
        auto conn_result = lease.emplace_connection(client_options(port));
        if (!conn_result) {
            co_return std::unexpected(conn_result.error());
        }
        auto &conn = **conn_result;
        auto connect_result = co_await conn.connect();
        if (!connect_result) {
            co_return std::unexpected(connect_result.error());
        }
    }
    co_return lease.get();
}

TEST(StealableHttp1ConnectionPoolSetTest, StealsIdleConnectionFromOtherLoopAndReturnsItHome) {
    fiber::event::EventLoopGroup server_group(1);
    auto server_state = std::make_shared<HoldServerState>();
    std::promise<std::uint16_t> server_port_promise;
    std::promise<fiber::common::IoErr> server_result_promise;
    auto server_port_future = server_port_promise.get_future();
    auto server_result_future = server_result_promise.get_future();

    server_group.start();
    fiber::async::spawn(server_group.at(0), [&]() {
        return run_hold_server(&server_group.at(0), 1, &server_port_promise, &server_result_promise, server_state);
    });

    const std::uint16_t port = server_port_future.get();
    ASSERT_NE(port, 0);

    fiber::event::EventLoopGroup group(2);
    fiber::http::StealableHttp1ConnectionPoolSet::Options options{};
    options.max_idle_per_group = 2;
    options.max_idle_total = 4;
    options.initial_group_capacity = 2;
    fiber::http::StealableHttp1ConnectionPoolSet set(group, options);
    ASSERT_TRUE(set.init());

    const auto key = fiber::http::Http1ConnectionGroupKey::from_ip(fiber::net::IpAddress::loopback_v4(), port,
                                                                   fiber::http::Http1ConnectionGroupKey::Scheme::Http);

    std::promise<fiber::http::Http1ClientConnection *> home_conn_promise;
    auto home_conn_future = home_conn_promise.get_future();
    std::promise<bool> final_promise;
    auto final_future = final_promise.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() -> DetachedTask {
        auto lease = co_await set.acquire(key);
        auto conn_result = co_await ensure_connected(lease, port);
        if (!conn_result) {
            home_conn_promise.set_value(nullptr);
            co_return;
        }
        home_conn_promise.set_value(*conn_result);
        lease.reset();
    });

    auto *home_conn = home_conn_future.get();
    ASSERT_NE(home_conn, nullptr);

    fiber::async::spawn(group.at(1), [&, home_conn]() -> DetachedTask {
        auto borrowed = co_await set.acquire(key);
        const bool borrowed_ok = borrowed.valid() && borrowed.hit() && borrowed.has_connection() &&
                                 borrowed.get() == home_conn && &borrowed.connection().loop() == &group.at(0);
        borrowed.reset();

        fiber::async::spawn(group.at(0), [&, borrowed_ok, home_conn]() -> DetachedTask {
            auto returned = co_await set.acquire(key);
            const bool returned_ok = returned.valid() && returned.hit() && returned.has_connection() &&
                                     returned.get() == home_conn;
            if (returned_ok) {
                returned.connection().close();
            }
            returned.reset();
            final_promise.set_value(borrowed_ok && returned_ok);
        });
    });

    EXPECT_TRUE(final_future.get());

    server_state->stop.store(true, std::memory_order_release);
    EXPECT_EQ(server_result_future.get(), fiber::common::IoErr::None);

    group.stop();
    group.join();
    server_group.stop();
    server_group.join();
}

TEST(StealableHttp1ConnectionPoolSetTest, LocalMissReturnsCallerLoopLeaseWhenNoRemoteHintMatches) {
    fiber::event::EventLoopGroup group(2);
    fiber::http::StealableHttp1ConnectionPoolSet set(group);
    ASSERT_TRUE(set.init());

    const auto key = fiber::http::Http1ConnectionGroupKey::from_ip(fiber::net::IpAddress::loopback_v4(), 80,
                                                                   fiber::http::Http1ConnectionGroupKey::Scheme::Http);

    std::promise<bool> result_promise;
    auto result_future = result_promise.get_future();

    group.start();
    fiber::async::spawn(group.at(1), [&]() -> DetachedTask {
        auto lease = co_await set.acquire(key);
        auto conn_result = lease.emplace_connection({});
        const bool ok = lease.valid() && !lease.hit() && conn_result.has_value() && lease.get() != nullptr &&
                        &lease.connection().loop() == &fiber::event::EventLoop::current();
        if (ok) {
            lease.connection().close();
        }
        lease.reset();
        result_promise.set_value(ok);
    });

    EXPECT_TRUE(result_future.get());
    group.stop();
    group.join();
}

} // namespace
