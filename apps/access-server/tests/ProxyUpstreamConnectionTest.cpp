#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <optional>
#include <sys/socket.h>
#include <utility>
#include <vector>

#include "async/Spawn.h"
#include "event/EventLoopGroup.h"
#include "execution/ProxyUpstreamConnection.h"
#include "http/Http1ClientConnection.h"
#include "http/Http1ConnectionGroupKey.h"
#include "net/SocketAddress.h"
#include "net/TcpListener.h"

namespace {

using namespace std::chrono_literals;

std::optional<std::uint16_t> bound_port(int fd) {
    sockaddr_storage bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&bound), &len) != 0) {
        return std::nullopt;
    }
    fiber::net::SocketAddress address;
    if (!fiber::net::SocketAddress::from_sockaddr(reinterpret_cast<sockaddr *>(&bound), len, address)) {
        return std::nullopt;
    }
    return address.port();
}

struct ResolverState {
    std::vector<fiber::net::IpAddress> addresses;
    std::size_t calls = 0;
};

fiber::async::Task<fiber::common::IoResult<std::vector<fiber::net::IpAddress>>>
resolve_addresses(void *context, std::string_view) noexcept {
    auto &state = *static_cast<ResolverState *>(context);
    ++state.calls;
    co_return state.addresses;
}

fiber::access_server::ProxyDnsResolver resolver_adapter(ResolverState &state) noexcept {
    return {
            .context = &state,
            .resolve = resolve_addresses,
    };
}

struct ConnectionScenarioResult {
    fiber::common::IoErr error = fiber::common::IoErr::None;
    fiber::access_server::ProxyConnectErrorCode error_code = fiber::access_server::ProxyConnectErrorCode::Connect;
    fiber::net::IpAddress connected_ip;
    std::size_t resolver_calls = 0;
    bool first_hit = false;
    bool second_hit = false;
};

fiber::async::DetachedTask run_ip_scenario(fiber::http::LocalHttp1ConnectionPoolSet *pool,
                                           fiber::http::Http1ConnectionGroupKey key, ResolverState *resolver,
                                           std::promise<ConnectionScenarioResult> *promise) {
    ConnectionScenarioResult result;
    auto connected = co_await fiber::access_server::acquire_proxy_upstream_connection(
            *pool, resolver_adapter(*resolver), key, 500ms);
    result.resolver_calls = resolver->calls;
    if (!connected) {
        result.error = connected.error().io_error;
        result.error_code = connected.error().code;
    } else {
        result.first_hit = connected->lease.hit();
        result.connected_ip = connected->connection->options().peer_addr.ip();
        connected->lease.reset();
    }
    co_await pool->shutdown_async();
    promise->set_value(std::move(result));
}

fiber::async::DetachedTask run_pool_hit_scenario(fiber::http::LocalHttp1ConnectionPoolSet *pool,
                                                 fiber::http::Http1ConnectionGroupKey key, ResolverState *resolver,
                                                 std::promise<ConnectionScenarioResult> *promise) {
    ConnectionScenarioResult result;
    auto first = co_await fiber::access_server::acquire_proxy_upstream_connection(*pool, resolver_adapter(*resolver),
                                                                                  key, 500ms);
    if (!first) {
        result.error = first.error().io_error;
        result.error_code = first.error().code;
        result.resolver_calls = resolver->calls;
        co_await pool->shutdown_async();
        promise->set_value(std::move(result));
        co_return;
    }
    result.first_hit = first->lease.hit();
    first->lease.reset();

    auto second = co_await fiber::access_server::acquire_proxy_upstream_connection(*pool, resolver_adapter(*resolver),
                                                                                   key, 500ms);
    result.resolver_calls = resolver->calls;
    if (!second) {
        result.error = second.error().io_error;
        result.error_code = second.error().code;
    } else {
        result.second_hit = second->lease.hit();
        second->lease.reset();
    }
    co_await pool->shutdown_async();
    promise->set_value(std::move(result));
}

fiber::async::DetachedTask run_shutdown_scenario(fiber::http::LocalHttp1ConnectionPoolSet *pool,
                                                 fiber::http::Http1ConnectionGroupKey key, ResolverState *resolver,
                                                 std::promise<ConnectionScenarioResult> *promise) {
    co_await pool->shutdown_async();
    ConnectionScenarioResult result;
    auto connected = co_await fiber::access_server::acquire_proxy_upstream_connection(
            *pool, resolver_adapter(*resolver), key, 500ms);
    result.resolver_calls = resolver->calls;
    if (!connected) {
        result.error = connected.error().io_error;
        result.error_code = connected.error().code;
    }
    promise->set_value(std::move(result));
}

TEST(ProxyUpstreamConnectionTest, IpKeyBypassesDns) {
    fiber::event::EventLoopGroup group(1);
    fiber::http::LocalHttp1ConnectionPoolSet pool(group);
    ASSERT_TRUE(pool.init());
    fiber::net::TcpListener listener(group.at(0));
    ASSERT_TRUE(listener.bind(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 0), {}));
    auto port = bound_port(listener.fd());
    ASSERT_TRUE(port);

    ResolverState resolver;
    std::promise<ConnectionScenarioResult> promise;
    auto future = promise.get_future();
    group.start();
    fiber::async::spawn(group.at(0), [&]() {
        return run_ip_scenario(
                &pool,
                fiber::http::Http1ConnectionGroupKey::from_ip(fiber::net::IpAddress::loopback_v4(), *port,
                                                              fiber::http::Http1ConnectionGroupKey::Scheme::Http),
                &resolver, &promise);
    });

    const ConnectionScenarioResult result = future.get();
    EXPECT_EQ(result.error, fiber::common::IoErr::None);
    EXPECT_EQ(result.resolver_calls, 0U);
    EXPECT_FALSE(result.first_hit);
    EXPECT_EQ(result.connected_ip, fiber::net::IpAddress::loopback_v4());
    listener.close();
    group.stop();
    group.join();
}

TEST(ProxyUpstreamConnectionTest, NameKeyTriesEveryResolvedAddress) {
    fiber::event::EventLoopGroup group(1);
    fiber::http::LocalHttp1ConnectionPoolSet pool(group);
    ASSERT_TRUE(pool.init());
    fiber::net::TcpListener listener(group.at(0));
    ASSERT_TRUE(listener.bind(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 0), {}));
    auto port = bound_port(listener.fd());
    ASSERT_TRUE(port);
    auto key = fiber::http::Http1ConnectionGroupKey::from_name("upstream.example", *port,
                                                               fiber::http::Http1ConnectionGroupKey::Scheme::Http);
    ASSERT_TRUE(key);

    ResolverState resolver{
            .addresses =
                    {
                            fiber::net::IpAddress::v4({127, 0, 0, 2}),
                            fiber::net::IpAddress::loopback_v4(),
                    },
    };
    std::promise<ConnectionScenarioResult> promise;
    auto future = promise.get_future();
    group.start();
    fiber::async::spawn(group.at(0), [&]() { return run_ip_scenario(&pool, *key, &resolver, &promise); });

    const ConnectionScenarioResult result = future.get();
    EXPECT_EQ(result.error, fiber::common::IoErr::None);
    EXPECT_EQ(result.resolver_calls, 1U);
    EXPECT_EQ(result.connected_ip, fiber::net::IpAddress::loopback_v4());
    listener.close();
    group.stop();
    group.join();
}

TEST(ProxyUpstreamConnectionTest, PoolHitBypassesDns) {
    fiber::event::EventLoopGroup group(1);
    fiber::http::LocalHttp1ConnectionPoolSet pool(group);
    ASSERT_TRUE(pool.init());
    fiber::net::TcpListener listener(group.at(0));
    ASSERT_TRUE(listener.bind(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 0), {}));
    auto port = bound_port(listener.fd());
    ASSERT_TRUE(port);
    auto key = fiber::http::Http1ConnectionGroupKey::from_name("upstream.example", *port,
                                                               fiber::http::Http1ConnectionGroupKey::Scheme::Http);
    ASSERT_TRUE(key);

    ResolverState resolver{
            .addresses = {fiber::net::IpAddress::loopback_v4()},
    };
    std::promise<ConnectionScenarioResult> promise;
    auto future = promise.get_future();
    group.start();
    fiber::async::spawn(group.at(0), [&]() { return run_pool_hit_scenario(&pool, *key, &resolver, &promise); });

    const ConnectionScenarioResult result = future.get();
    EXPECT_EQ(result.error, fiber::common::IoErr::None);
    EXPECT_EQ(result.resolver_calls, 1U);
    EXPECT_FALSE(result.first_hit);
    EXPECT_TRUE(result.second_hit);
    listener.close();
    group.stop();
    group.join();
}

TEST(ProxyUpstreamConnectionTest, ReportsPoolShutdownBeforeDns) {
    fiber::event::EventLoopGroup group(1);
    fiber::http::LocalHttp1ConnectionPoolSet pool(group);
    ASSERT_TRUE(pool.init());
    auto key = fiber::http::Http1ConnectionGroupKey::from_name("upstream.example", 80,
                                                               fiber::http::Http1ConnectionGroupKey::Scheme::Http);
    ASSERT_TRUE(key);

    ResolverState resolver{
            .addresses = {fiber::net::IpAddress::loopback_v4()},
    };
    std::promise<ConnectionScenarioResult> promise;
    auto future = promise.get_future();
    group.start();
    fiber::async::spawn(group.at(0), [&]() { return run_shutdown_scenario(&pool, *key, &resolver, &promise); });

    const ConnectionScenarioResult result = future.get();
    EXPECT_EQ(result.error_code, fiber::access_server::ProxyConnectErrorCode::PoolShutdown);
    EXPECT_EQ(result.error, fiber::common::IoErr::Canceled);
    EXPECT_EQ(result.resolver_calls, 0U);
    group.stop();
    group.join();
}

} // namespace
