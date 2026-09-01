#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <vector>

#include <unistd.h>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/common/IoError.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/Http1ConnectionGroupKey.h>
#include <fiber/http/Http1ConnectionPoolCore.h>
#include <fiber/net/TcpListener.h>

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

fiber::http::Http1ClientConnectionOptions client_options(std::uint16_t port,
                                                         fiber::http::Http1ConnectionPoolAffinity pool_affinity = {}) {
    fiber::http::Http1ClientConnectionOptions options;
    options.peer_addr = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port);
    options.pool_affinity = pool_affinity;
    return options;
}

struct HoldServerState {
    std::atomic_bool stop{false};
};

DetachedTask run_hold_server(fiber::event::EventLoop *loop, std::size_t accept_count,
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
            for (int fd: accepted_fds) {
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

    for (int fd: accepted_fds) {
        ::close(fd);
    }
    result_promise->set_value(fiber::common::IoErr::None);
}

fiber::async::Task<fiber::common::IoResult<fiber::http::Http1ClientConnection *>>
ensure_connected(fiber::http::Http1ConnectionPoolCore::Lease &lease, std::uint16_t port) {
    if (!lease.valid()) {
        co_return std::unexpected(fiber::common::IoErr::NoMem);
    }
    if (!lease.has_connection()) {
        auto conn_result = lease.emplace_connection(client_options(port, lease.key().pool_affinity()));
        if (!conn_result) {
            co_return std::unexpected(conn_result.error());
        }
        auto &conn = **conn_result;
        auto connect_result = co_await conn.connect(5s);
        if (!connect_result) {
            co_return std::unexpected(connect_result.error());
        }
    }
    co_return lease.get();
}

struct LifoScenarioResult {
    fiber::common::IoErr err = fiber::common::IoErr::None;
    bool first_miss = false;
    bool second_miss = false;
    bool first_hit_newest = false;
    bool second_hit_oldest = false;
    bool third_is_miss = false;
    std::size_t idle_after_release = 0;
    std::size_t groups_after_release = 0;
    std::size_t idle_while_leased = 0;
    std::size_t groups_while_leased = 0;
};

DetachedTask run_lifo_scenario(fiber::event::EventLoop *loop, std::uint16_t port,
                               std::promise<LifoScenarioResult> *promise) {
    LifoScenarioResult out;
    fiber::http::Http1ConnectionPoolCore pool(*loop, {
                                                             .max_idle_per_group = 2,
                                                             .max_idle_total = 4,
                                                             .idle_timeout = 30s,
                                                             .initial_group_capacity = 2,
                                                     });
    if (!pool.init()) {
        out.err = fiber::common::IoErr::NoMem;
        promise->set_value(out);
        co_return;
    }

    const auto key = fiber::http::Http1ConnectionGroupKey::from_ip(fiber::net::IpAddress::loopback_v4(), port,
                                                                   fiber::http::Http1ConnectionGroupKey::Scheme::Http);

    auto lease1 = pool.acquire(key);
    auto conn1_result = co_await ensure_connected(lease1, port);
    if (!conn1_result) {
        out.err = conn1_result.error();
        promise->set_value(out);
        co_return;
    }
    auto *oldest = *conn1_result;
    out.first_miss = !lease1.hit();

    auto lease2 = pool.acquire(key);
    auto conn2_result = co_await ensure_connected(lease2, port);
    if (!conn2_result) {
        out.err = conn2_result.error();
        promise->set_value(out);
        co_return;
    }
    auto *newest = *conn2_result;
    out.second_miss = !lease2.hit();

    lease1.reset();
    lease2.reset();
    out.idle_after_release = pool.idle_total();
    out.groups_after_release = pool.group_count();

    auto hit1 = pool.acquire(key);
    auto hit2 = pool.acquire(key);
    auto miss = pool.acquire(key);
    out.first_hit_newest = hit1.hit() && hit1.get() == newest;
    out.second_hit_oldest = hit2.hit() && hit2.get() == oldest;
    out.third_is_miss = miss.valid() && !miss.hit() && !miss.has_connection();
    out.idle_while_leased = pool.idle_total();
    out.groups_while_leased = pool.group_count();
    promise->set_value(out);
}

struct PerGroupEvictionResult {
    fiber::common::IoErr err = fiber::common::IoErr::None;
    bool kept_newest = false;
    bool oldest_evicted = false;
    std::size_t idle_after_release = 0;
    std::size_t groups_after_release = 0;
};

DetachedTask run_per_group_eviction_scenario(fiber::event::EventLoop *loop, std::uint16_t port,
                                             std::promise<PerGroupEvictionResult> *promise) {
    PerGroupEvictionResult out;
    fiber::http::Http1ConnectionPoolCore pool(*loop, {
                                                             .max_idle_per_group = 1,
                                                             .max_idle_total = 4,
                                                             .idle_timeout = 30s,
                                                             .initial_group_capacity = 1,
                                                     });
    if (!pool.init()) {
        out.err = fiber::common::IoErr::NoMem;
        promise->set_value(out);
        co_return;
    }

    const auto key = fiber::http::Http1ConnectionGroupKey::from_ip(fiber::net::IpAddress::loopback_v4(), port,
                                                                   fiber::http::Http1ConnectionGroupKey::Scheme::Http);

    auto lease1 = pool.acquire(key);
    auto conn1_result = co_await ensure_connected(lease1, port);
    if (!conn1_result) {
        out.err = conn1_result.error();
        promise->set_value(out);
        co_return;
    }

    auto lease2 = pool.acquire(key);
    auto conn2_result = co_await ensure_connected(lease2, port);
    if (!conn2_result) {
        out.err = conn2_result.error();
        promise->set_value(out);
        co_return;
    }
    auto *newest = *conn2_result;

    lease1.reset();
    lease2.reset();
    out.idle_after_release = pool.idle_total();
    out.groups_after_release = pool.group_count();

    auto hit = pool.acquire(key);
    auto miss = pool.acquire(key);
    out.kept_newest = hit.hit() && hit.get() == newest;
    out.oldest_evicted = miss.valid() && !miss.hit() && !miss.has_connection();
    promise->set_value(out);
}

struct GlobalEvictionResult {
    fiber::common::IoErr err = fiber::common::IoErr::None;
    bool group1_kept_newest = false;
    bool group1_oldest_evicted = false;
    bool group2_preserved = false;
    std::size_t idle_after_release = 0;
    std::size_t groups_after_release = 0;
};

DetachedTask run_global_eviction_scenario(fiber::event::EventLoop *loop, std::uint16_t port1, std::uint16_t port2,
                                          std::promise<GlobalEvictionResult> *promise) {
    GlobalEvictionResult out;
    fiber::http::Http1ConnectionPoolCore pool(*loop, {
                                                             .max_idle_per_group = 2,
                                                             .max_idle_total = 2,
                                                             .idle_timeout = 30s,
                                                             .initial_group_capacity = 2,
                                                     });
    if (!pool.init()) {
        out.err = fiber::common::IoErr::NoMem;
        promise->set_value(out);
        co_return;
    }

    const auto key1 = fiber::http::Http1ConnectionGroupKey::from_ip(fiber::net::IpAddress::loopback_v4(), port1,
                                                                    fiber::http::Http1ConnectionGroupKey::Scheme::Http);
    const auto key2 = fiber::http::Http1ConnectionGroupKey::from_ip(fiber::net::IpAddress::loopback_v4(), port2,
                                                                    fiber::http::Http1ConnectionGroupKey::Scheme::Http);

    auto lease1 = pool.acquire(key1);
    auto conn1_result = co_await ensure_connected(lease1, port1);
    if (!conn1_result) {
        out.err = conn1_result.error();
        promise->set_value(out);
        co_return;
    }

    auto lease2 = pool.acquire(key2);
    auto conn2_result = co_await ensure_connected(lease2, port2);
    if (!conn2_result) {
        out.err = conn2_result.error();
        promise->set_value(out);
        co_return;
    }
    auto *group2_conn = *conn2_result;

    auto lease3 = pool.acquire(key1);
    auto conn3_result = co_await ensure_connected(lease3, port1);
    if (!conn3_result) {
        out.err = conn3_result.error();
        promise->set_value(out);
        co_return;
    }
    auto *group1_newest = *conn3_result;

    lease1.reset();
    lease2.reset();
    lease3.reset();
    out.idle_after_release = pool.idle_total();
    out.groups_after_release = pool.group_count();

    auto key1_hit = pool.acquire(key1);
    auto key1_miss = pool.acquire(key1);
    auto key2_hit = pool.acquire(key2);
    out.group1_kept_newest = key1_hit.hit() && key1_hit.get() == group1_newest;
    out.group1_oldest_evicted = key1_miss.valid() && !key1_miss.hit() && !key1_miss.has_connection();
    out.group2_preserved = key2_hit.hit() && key2_hit.get() == group2_conn;
    promise->set_value(out);
}

struct ExpireScenarioResult {
    fiber::common::IoErr err = fiber::common::IoErr::None;
    bool expired = false;
    std::size_t idle_after_sweep = 0;
    std::size_t groups_after_sweep = 0;
};

DetachedTask run_expire_scenario(fiber::event::EventLoop *loop, std::uint16_t port,
                                 std::promise<ExpireScenarioResult> *promise) {
    ExpireScenarioResult out;
    fiber::http::Http1ConnectionPoolCore pool(*loop, {
                                                             .max_idle_per_group = 1,
                                                             .max_idle_total = 1,
                                                             .idle_timeout = 10ms,
                                                             .initial_group_capacity = 1,
                                                     });
    if (!pool.init()) {
        out.err = fiber::common::IoErr::NoMem;
        promise->set_value(out);
        co_return;
    }

    const auto key = fiber::http::Http1ConnectionGroupKey::from_ip(fiber::net::IpAddress::loopback_v4(), port,
                                                                   fiber::http::Http1ConnectionGroupKey::Scheme::Http);
    auto lease = pool.acquire(key);
    auto conn_result = co_await ensure_connected(lease, port);
    if (!conn_result) {
        out.err = conn_result.error();
        promise->set_value(out);
        co_return;
    }

    lease.reset();
    co_await fiber::async::sleep(30ms);

    auto miss = pool.acquire(key);
    out.expired = miss.valid() && !miss.hit() && !miss.has_connection();
    out.idle_after_sweep = pool.idle_total();
    out.groups_after_sweep = pool.group_count();
    promise->set_value(out);
}

struct ClosedScenarioResult {
    fiber::common::IoErr err = fiber::common::IoErr::None;
    bool dropped = false;
    std::size_t idle_after_release = 0;
    std::size_t groups_after_release = 0;
};

DetachedTask run_closed_scenario(fiber::event::EventLoop *loop, std::uint16_t port,
                                 std::promise<ClosedScenarioResult> *promise) {
    ClosedScenarioResult out;
    fiber::http::Http1ConnectionPoolCore pool(*loop, {
                                                             .max_idle_per_group = 1,
                                                             .max_idle_total = 1,
                                                             .idle_timeout = 30s,
                                                             .initial_group_capacity = 1,
                                                     });
    if (!pool.init()) {
        out.err = fiber::common::IoErr::NoMem;
        promise->set_value(out);
        co_return;
    }

    const auto key = fiber::http::Http1ConnectionGroupKey::from_ip(fiber::net::IpAddress::loopback_v4(), port,
                                                                   fiber::http::Http1ConnectionGroupKey::Scheme::Http);
    auto lease = pool.acquire(key);
    auto conn_result = co_await ensure_connected(lease, port);
    if (!conn_result) {
        out.err = conn_result.error();
        promise->set_value(out);
        co_return;
    }

    lease.connection().close();
    lease.reset();
    out.idle_after_release = pool.idle_total();
    out.groups_after_release = pool.group_count();

    auto miss = pool.acquire(key);
    out.dropped = miss.valid() && !miss.hit() && !miss.has_connection();
    promise->set_value(out);
}

struct AffinityScenarioResult {
    fiber::common::IoErr err = fiber::common::IoErr::None;
    bool mismatched_options_rejected = false;
    bool matching_options_recorded = false;
    bool different_identity_missed = false;
    bool original_identity_reused = false;
};

DetachedTask run_affinity_scenario(fiber::event::EventLoop *loop, std::uint16_t port,
                                   std::promise<AffinityScenarioResult> *promise) {
    AffinityScenarioResult out;
    fiber::http::Http1ConnectionPoolCore pool(*loop, {
                                                             .max_idle_per_group = 2,
                                                             .max_idle_total = 4,
                                                             .idle_timeout = 30s,
                                                             .initial_group_capacity = 2,
                                                     });
    if (!pool.init()) {
        out.err = fiber::common::IoErr::NoMem;
        promise->set_value(out);
        co_return;
    }

    const auto first_key = fiber::http::Http1ConnectionGroupKey::from_ip(
            fiber::net::IpAddress::loopback_v4(), port, fiber::http::Http1ConnectionGroupKey::Scheme::Http,
            fiber::http::Http1ConnectionPoolAffinity{41});
    const auto second_key = fiber::http::Http1ConnectionGroupKey::from_ip(
            fiber::net::IpAddress::loopback_v4(), port, fiber::http::Http1ConnectionGroupKey::Scheme::Http,
            fiber::http::Http1ConnectionPoolAffinity{42});

    auto first_lease = pool.acquire(first_key);
    auto mismatched_options = first_lease.emplace_connection(client_options(port, second_key.pool_affinity()));
    out.mismatched_options_rejected = !mismatched_options &&
                                      mismatched_options.error() == fiber::common::IoErr::Invalid &&
                                      !first_lease.has_connection();
    auto connection_result = co_await ensure_connected(first_lease, port);
    if (!connection_result) {
        out.err = connection_result.error();
        promise->set_value(out);
        co_return;
    }
    auto *first_connection = *connection_result;
    out.matching_options_recorded = first_connection->options().pool_affinity == first_key.pool_affinity();
    first_lease.reset();

    auto other_identity = pool.acquire(second_key);
    out.different_identity_missed = other_identity.valid() && !other_identity.hit() && !other_identity.has_connection();
    other_identity.reset();

    auto original_identity = pool.acquire(first_key);
    out.original_identity_reused = original_identity.hit() && original_identity.get() == first_connection;
    if (original_identity.has_connection()) {
        original_identity.connection().close();
    }
    promise->set_value(out);
}

TEST(Http1ConnectionPoolTest, LeaseCanBuildNewConnectionAndReuseGroupInLifoOrder) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    auto state = std::make_shared<HoldServerState>();
    std::promise<std::uint16_t> port_promise;
    auto port_future = port_promise.get_future();
    std::promise<fiber::common::IoErr> server_result_promise;
    auto server_result_future = server_result_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_hold_server(&group.at(0), 2, &port_promise, &server_result_promise, state);
    });

    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    std::promise<LifoScenarioResult> result_promise;
    auto result_future = result_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return run_lifo_scenario(&group.at(0), port, &result_promise); });

    const LifoScenarioResult result = result_future.get();
    EXPECT_EQ(result.err, fiber::common::IoErr::None);
    EXPECT_TRUE(result.first_miss);
    EXPECT_TRUE(result.second_miss);
    EXPECT_TRUE(result.first_hit_newest);
    EXPECT_TRUE(result.second_hit_oldest);
    EXPECT_TRUE(result.third_is_miss);
    EXPECT_EQ(result.idle_after_release, 2u);
    EXPECT_EQ(result.groups_after_release, 1u);
    EXPECT_EQ(result.idle_while_leased, 0u);
    EXPECT_EQ(result.groups_while_leased, 0u);

    state->stop.store(true, std::memory_order_release);
    EXPECT_EQ(server_result_future.get(), fiber::common::IoErr::None);
    group.stop();
    group.join();
}

TEST(Http1ConnectionPoolTest, PoolAffinityRejectsMismatchedOptionsAndPreventsCrossReuse) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    auto state = std::make_shared<HoldServerState>();
    std::promise<std::uint16_t> port_promise;
    auto port_future = port_promise.get_future();
    std::promise<fiber::common::IoErr> server_result_promise;
    auto server_result_future = server_result_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_hold_server(&group.at(0), 1, &port_promise, &server_result_promise, state);
    });

    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    std::promise<AffinityScenarioResult> result_promise;
    auto result_future = result_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return run_affinity_scenario(&group.at(0), port, &result_promise); });

    const auto result = result_future.get();
    EXPECT_EQ(result.err, fiber::common::IoErr::None);
    EXPECT_TRUE(result.mismatched_options_rejected);
    EXPECT_TRUE(result.matching_options_recorded);
    EXPECT_TRUE(result.different_identity_missed);
    EXPECT_TRUE(result.original_identity_reused);

    state->stop.store(true, std::memory_order_release);
    EXPECT_EQ(server_result_future.get(), fiber::common::IoErr::None);
    group.stop();
    group.join();
}

TEST(Http1ConnectionPoolTest, PerGroupLimitEvictsOldestIdleConnection) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    auto state = std::make_shared<HoldServerState>();
    std::promise<std::uint16_t> port_promise;
    auto port_future = port_promise.get_future();
    std::promise<fiber::common::IoErr> server_result_promise;
    auto server_result_future = server_result_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_hold_server(&group.at(0), 2, &port_promise, &server_result_promise, state);
    });

    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    std::promise<PerGroupEvictionResult> result_promise;
    auto result_future = result_promise.get_future();
    fiber::async::spawn(group.at(0),
                        [&]() { return run_per_group_eviction_scenario(&group.at(0), port, &result_promise); });

    const PerGroupEvictionResult result = result_future.get();
    EXPECT_EQ(result.err, fiber::common::IoErr::None);
    EXPECT_TRUE(result.kept_newest);
    EXPECT_TRUE(result.oldest_evicted);
    EXPECT_EQ(result.idle_after_release, 1u);
    EXPECT_EQ(result.groups_after_release, 1u);

    state->stop.store(true, std::memory_order_release);
    EXPECT_EQ(server_result_future.get(), fiber::common::IoErr::None);
    group.stop();
    group.join();
}

TEST(Http1ConnectionPoolTest, GlobalLimitEvictsOldestIdleConnectionAcrossGroups) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    auto state1 = std::make_shared<HoldServerState>();
    std::promise<std::uint16_t> port1_promise;
    auto port1_future = port1_promise.get_future();
    std::promise<fiber::common::IoErr> server1_result_promise;
    auto server1_result_future = server1_result_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_hold_server(&group.at(0), 2, &port1_promise, &server1_result_promise, state1);
    });

    auto state2 = std::make_shared<HoldServerState>();
    std::promise<std::uint16_t> port2_promise;
    auto port2_future = port2_promise.get_future();
    std::promise<fiber::common::IoErr> server2_result_promise;
    auto server2_result_future = server2_result_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_hold_server(&group.at(0), 1, &port2_promise, &server2_result_promise, state2);
    });

    const std::uint16_t port1 = port1_future.get();
    const std::uint16_t port2 = port2_future.get();
    ASSERT_NE(port1, 0);
    ASSERT_NE(port2, 0);

    std::promise<GlobalEvictionResult> result_promise;
    auto result_future = result_promise.get_future();
    fiber::async::spawn(group.at(0),
                        [&]() { return run_global_eviction_scenario(&group.at(0), port1, port2, &result_promise); });

    const GlobalEvictionResult result = result_future.get();
    EXPECT_EQ(result.err, fiber::common::IoErr::None);
    EXPECT_TRUE(result.group1_kept_newest);
    EXPECT_TRUE(result.group1_oldest_evicted);
    EXPECT_TRUE(result.group2_preserved);
    EXPECT_EQ(result.idle_after_release, 2u);
    EXPECT_EQ(result.groups_after_release, 2u);

    state1->stop.store(true, std::memory_order_release);
    state2->stop.store(true, std::memory_order_release);
    EXPECT_EQ(server1_result_future.get(), fiber::common::IoErr::None);
    EXPECT_EQ(server2_result_future.get(), fiber::common::IoErr::None);
    group.stop();
    group.join();
}

TEST(Http1ConnectionPoolTest, IdleExpiryTimerRemovesIdleConnectionsAndBuckets) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    auto state = std::make_shared<HoldServerState>();
    std::promise<std::uint16_t> port_promise;
    auto port_future = port_promise.get_future();
    std::promise<fiber::common::IoErr> server_result_promise;
    auto server_result_future = server_result_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_hold_server(&group.at(0), 1, &port_promise, &server_result_promise, state);
    });

    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    std::promise<ExpireScenarioResult> result_promise;
    auto result_future = result_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return run_expire_scenario(&group.at(0), port, &result_promise); });

    const ExpireScenarioResult result = result_future.get();
    EXPECT_EQ(result.err, fiber::common::IoErr::None);
    EXPECT_TRUE(result.expired);
    EXPECT_EQ(result.idle_after_sweep, 0u);
    EXPECT_EQ(result.groups_after_sweep, 0u);

    state->stop.store(true, std::memory_order_release);
    EXPECT_EQ(server_result_future.get(), fiber::common::IoErr::None);
    group.stop();
    group.join();
}

TEST(Http1ConnectionPoolTest, ClosedConnectionIsNotReturnedToPoolOnLeaseReset) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    auto state = std::make_shared<HoldServerState>();
    std::promise<std::uint16_t> port_promise;
    auto port_future = port_promise.get_future();
    std::promise<fiber::common::IoErr> server_result_promise;
    auto server_result_future = server_result_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_hold_server(&group.at(0), 1, &port_promise, &server_result_promise, state);
    });

    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    std::promise<ClosedScenarioResult> result_promise;
    auto result_future = result_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return run_closed_scenario(&group.at(0), port, &result_promise); });

    const ClosedScenarioResult result = result_future.get();
    EXPECT_EQ(result.err, fiber::common::IoErr::None);
    EXPECT_TRUE(result.dropped);
    EXPECT_EQ(result.idle_after_release, 0u);
    EXPECT_EQ(result.groups_after_release, 0u);

    state->stop.store(true, std::memory_order_release);
    EXPECT_EQ(server_result_future.get(), fiber::common::IoErr::None);
    group.stop();
    group.join();
}

} // namespace
