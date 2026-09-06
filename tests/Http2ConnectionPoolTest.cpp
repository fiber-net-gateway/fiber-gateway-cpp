#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/async/TaskSelect.h>
#include <fiber/async/Timeout.h>
#include <fiber/async/Yield.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/Http2PooledExchange.h>
#include <fiber/http/LocalHttp2ConnectionPoolSet.h>
#include <fiber/http/ServerRequestFactory.h>
#include <fiber/net/TcpListener.h>
#include <functional>
#include <future>
#include <memory>
#include <netinet/in.h>
#include <vector>

namespace {
using namespace std::chrono_literals;
using namespace fiber;
using async::DetachedTask;
using async::Task;
using common::IoErr;
using http::Http2ConnectionPoolCore;
using http::HttpConnectionGroupKey;
using Lease = Http2ConnectionPoolCore::Lease;

struct PoolHarness {
    struct Server {
        Server(http::Http2Connection::Options options, http::ServerRequestFactory &factory) :
            conn(options, &factory, http::ServerRequestFactory::ops()) {
            gate.arm(conn);
        }
        http::Http2Connection conn;
        http::Http2CloseGate gate;
    };
    PoolHarness(event::EventLoop &loop, Http2ConnectionPoolCore::Options options, std::uint32_t server_max) :
        loop(loop), pool(loop, options), listener(loop), factory({}, [this](http::HttpExchange &ex) -> Task<void> {
            ++requests;
            while (hold_responses)
                co_await async::sleep(1ms);
            auto sent = co_await ex.send_header({.kind = http::OutgoingHeaderKind::Final,
                                                 .status_code = 204,
                                                 .body = http::ResponseBodySpec::None(),
                                                 .end_stream = true},
                                                1s);
            if (sent)
                ++responses;
        }) {
        server_options.local_max_concurrent_streams = server_max;
        EXPECT_TRUE(pool.init());
        auto bound = listener.bind(net::SocketAddress(net::IpAddress::loopback_v4(), 0), {});
        EXPECT_TRUE(bound);
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        EXPECT_EQ(::getsockname(listener.fd(), reinterpret_cast<sockaddr *>(&addr), &len), 0);
        port = ntohs(addr.sin_port);
        async::spawn(loop, [this]() { return accept_loop(); });
    }
    HttpConnectionGroupKey key(std::uint16_t identity = 0) const {
        return HttpConnectionGroupKey::from_ip(net::IpAddress::loopback_v4(), identity ? identity : port,
                                               HttpConnectionGroupKey::Scheme::Http);
    }
    Http2ConnectionPoolCore::Connector connector() { return {&PoolHarness::connect, this}; }
    static Task<common::IoResult<void>> connect(void *ctx, http::Http2ClientConnection &conn,
                                                const HttpConnectionGroupKey &) noexcept {
        auto &h = *static_cast<PoolHarness *>(ctx);
        ++h.dials;
        ++h.dials_active;
        h.max_dials_active = std::max(h.max_dials_active, h.dials_active);
        struct Guard {
            PoolHarness &h;
            ~Guard() { --h.dials_active; }
        } guard{h};
        if (h.dial_delay > 0ms)
            co_await async::sleep(h.dial_delay);
        if (h.fail_dials > 0) {
            --h.fail_dials;
            co_return std::unexpected(IoErr::ConnRefused);
        }
        co_return co_await conn.connect(net::SocketAddress(net::IpAddress::loopback_v4(), h.port), 1s);
    }
    Task<common::IoResult<Lease>> acquire(std::chrono::milliseconds timeout = 1s) {
        return pool.acquire(key(), connector(), timeout);
    }
    DetachedTask accept_loop() {
        for (;;) {
            auto accepted = co_await listener.accept();
            if (!accepted)
                break;
            auto transport = http::TcpTransport::create(loop, std::move(*accepted), net::kNoDelayTcpSocketOptions);
            EXPECT_TRUE(transport);
            if (!transport)
                break;
            auto server = std::make_unique<Server>(server_options, factory);
            EXPECT_EQ(server->conn.start(std::move(*transport)), IoErr::None);
            servers.push_back(std::move(server));
        }
        accept_done = true;
    }
    Task<void> settings(Lease &lease, std::uint32_t value) {
        EXPECT_FALSE(servers.empty());
        // Send a real SETTINGS frame over the idle server transport. Its initial
        // SETTINGS has already drained when this helper is called.
        const std::array<std::uint8_t, 15> frame{0,
                                                 0,
                                                 6,
                                                 4,
                                                 0,
                                                 0,
                                                 0,
                                                 0,
                                                 0,
                                                 0,
                                                 3,
                                                 static_cast<std::uint8_t>(value >> 24),
                                                 static_cast<std::uint8_t>(value >> 16),
                                                 static_cast<std::uint8_t>(value >> 8),
                                                 static_cast<std::uint8_t>(value)};
        auto result = co_await servers.front()->conn.transport().write(frame.data(), frame.size(), 1s);
        EXPECT_TRUE(result);
        for (int i = 0; i < 1000 && lease.connection().http2().peer_max_concurrent_streams() != value; ++i)
            co_await async::sleep(1ms);
        EXPECT_EQ(lease.connection().http2().peer_max_concurrent_streams(), value);
    }
    Task<void> settled(Lease &lease) {
        for (int i = 0; i < 1000 && !lease.connection().http2().peer_settings_received(); ++i)
            co_await async::sleep(1ms);
        EXPECT_TRUE(lease.connection().http2().peer_settings_received());
    }
    Task<void> close() {
        hold_responses = false;
        pool.shutdown();
        co_await pool.join();
        EXPECT_EQ(pool.connection_total(), 0u);
        EXPECT_EQ(pool.group_count(), 0u);
        listener.close();
        while (!accept_done)
            co_await async::sleep(1ms);
        for (auto &server: servers) {
            server->conn.shutdown();
            (void) co_await server->gate.join();
        }
    }
    event::EventLoop &loop;
    Http2ConnectionPoolCore pool;
    net::TcpListener listener;
    http::ServerRequestFactory factory;
    http::Http2Connection::Options server_options{};
    std::vector<std::unique_ptr<Server>> servers;
    std::uint16_t port = 0;
    unsigned dials = 0, dials_active = 0, max_dials_active = 0, fail_dials = 0;
    unsigned requests = 0, responses = 0;
    std::chrono::milliseconds dial_delay{};
    bool hold_responses = false, accept_done = false;
};

using Scenario = std::function<Task<void>(PoolHarness &)>;
DetachedTask run_scenario(event::EventLoop &loop, Http2ConnectionPoolCore::Options options, std::uint32_t server_max,
                          Scenario scenario, std::promise<void> &done) {
    {
        PoolHarness harness(loop, options, server_max);
        co_await scenario(harness);
        co_await harness.close();
    }
    done.set_value();
}
void run_case(Scenario scenario, Http2ConnectionPoolCore::Options options = {}, std::uint32_t server_max = 100) {
    event::EventLoopGroup group(1);
    group.start();
    std::promise<void> done;
    auto future = done.get_future();
    async::spawn(group.at(0),
                 [&]() { return run_scenario(group.at(0), options, server_max, std::move(scenario), done); });
    // A hung coroutine is a test failure with a bounded process lifetime.
    if (future.wait_for(15s) != std::future_status::ready) {
        ADD_FAILURE() << "HTTP/2 pool scenario did not finish";
        std::abort();
    }
    future.get();
    group.stop();
    group.join();
}
Http2ConnectionPoolCore::Options single_connection(std::size_t streams = 1) {
    Http2ConnectionPoolCore::Options options;
    options.max_connections_per_group = 1;
    options.max_streams_per_connection = streams;
    return options;
}

TEST(Http2ConnectionPoolTest, ConcurrentRequestsShareSingleDialAndConnection) {
    run_case([](PoolHarness &h) -> Task<void> {
        h.dial_delay = 10ms;
        h.hold_responses = true;
        unsigned done = 0;
        for (int i = 0; i < 12; ++i) {
            async::spawn(h.loop, [&]() -> DetachedTask {
                auto lease = co_await h.acquire();
                EXPECT_TRUE(lease);
                if (lease) {
                    mem::BufPool buffers;
                    http::Http2PooledExchange ex(std::move(*lease), buffers);
                    auto sent = co_await ex->send_request_header(
                            {.method = http::HttpMethod::Get, .scheme = "http", .authority = "localhost", .path = "/"},
                            true, 1s);
                    EXPECT_TRUE(sent);
                    auto response = co_await ex->read_header(1s);
                    EXPECT_TRUE(response);
                    if (response)
                        EXPECT_EQ((*response)->status_code, 204);
                }
                ++done;
            });
        }
        while (h.requests != 12)
            co_await async::sleep(1ms);
        EXPECT_EQ(h.dials, 1u);
        EXPECT_EQ(h.servers.size(), 1u);
        EXPECT_EQ(h.max_dials_active, 1u);
        h.hold_responses = false;
        while (done != 12)
            co_await async::sleep(1ms);
        EXPECT_EQ(h.pool.idle_total(), 1u);
    });
}

TEST(Http2ConnectionPoolTest, SaturationWaitsAndReturnsSameConnection) {
    run_case(
            [](PoolHarness &h) -> Task<void> {
                auto a = co_await h.acquire();
                EXPECT_TRUE(a);
                if (!a)
                    co_return;
                co_await h.settled(*a);
                auto *conn = &a->connection();
                auto b = co_await h.acquire();
                EXPECT_TRUE(b);
                if (!b)
                    co_return;
                auto poll = co_await h.acquire(0ms);
                EXPECT_FALSE(poll);
                EXPECT_EQ(poll.error(), IoErr::Busy);
                bool acquired = false;
                async::spawn(h.loop, [&]() -> DetachedTask {
                    auto c = co_await h.acquire();
                    EXPECT_TRUE(c);
                    if (c)
                        EXPECT_EQ(&c->connection(), conn);
                    acquired = true;
                });
                co_await async::sleep(5ms);
                EXPECT_FALSE(acquired);
                a->reset();
                while (!acquired)
                    co_await async::sleep(1ms);
                EXPECT_EQ(h.dials, 1u);
            },
            single_connection(0), 2);
}

TEST(Http2ConnectionPoolTest, SaturatedConnectionReturnsToHeadWithoutReorderingOthers) {
    auto options = single_connection();
    options.max_connections_per_group = 3;
    run_case(
            [](PoolHarness &h) -> Task<void> {
                auto a = co_await h.acquire();
                auto b = co_await h.acquire();
                auto c = co_await h.acquire();
                EXPECT_TRUE(a && b && c);
                if (!a || !b || !c)
                    co_return;
                auto *first = &a->connection();
                auto *second = &b->connection();
                EXPECT_NE(first, second);
                b->reset();
                a->reset();
                auto next = co_await h.acquire();
                EXPECT_TRUE(next);
                if (next)
                    EXPECT_EQ(&next->connection(), first);
                EXPECT_EQ(h.dials, 3u);
            },
            options);
}

TEST(Http2ConnectionPoolTest, WaitersAreFifoAndNewPollCannotBarge) {
    run_case(
            [](PoolHarness &h) -> Task<void> {
                auto lease = co_await h.acquire();
                EXPECT_TRUE(lease);
                if (!lease)
                    co_return;
                std::vector<int> order;
                for (int i = 0; i < 5; ++i) {
                    async::spawn(h.loop, [&, i]() -> DetachedTask {
                        auto next = co_await h.acquire();
                        EXPECT_TRUE(next);
                        order.push_back(i);
                    });
                }
                co_await async::sleep(5ms);
                lease->reset();
                auto barging = co_await h.acquire(0ms);
                EXPECT_FALSE(barging);
                EXPECT_EQ(barging.error(), IoErr::Busy);
                while (order.size() != 5)
                    co_await async::sleep(1ms);
                EXPECT_EQ(order, (std::vector<int>{0, 1, 2, 3, 4}));
            },
            single_connection());
}

TEST(Http2ConnectionPoolTest, TimeoutUnlinksWaiterAndLaterAcquireSucceeds) {
    run_case(
            [](PoolHarness &h) -> Task<void> {
                auto lease = co_await h.acquire();
                EXPECT_TRUE(lease);
                if (!lease)
                    co_return;
                const auto start = h.loop.now();
                auto result = co_await h.acquire(50ms);
                EXPECT_FALSE(result);
                EXPECT_EQ(result.error(), IoErr::TimedOut);
                EXPECT_GE(h.loop.now() - start, 45ms);
                lease->reset();
                auto next = co_await h.acquire(0ms);
                EXPECT_TRUE(next);
                EXPECT_EQ(h.dials, 1u);
            },
            single_connection());
}

TEST(Http2ConnectionPoolTest, IdleExpiryClosesAndRecyclesBucket) {
    auto options = single_connection();
    options.idle_timeout = 15ms;
    run_case(
            [](PoolHarness &h) -> Task<void> {
                auto lease = co_await h.acquire();
                EXPECT_TRUE(lease);
                if (!lease)
                    co_return;
                co_await h.settled(*lease);
                co_await async::sleep(25ms);
                EXPECT_EQ(h.pool.connection_total(), 1u); // A held lease never expires.
                lease->reset();
                co_await h.pool.join();
                EXPECT_EQ(h.pool.group_count(), 0u);
                EXPECT_EQ(h.pool.idle_total(), 0u);
                (void) co_await h.servers.front()->gate.join();
                EXPECT_TRUE(h.servers.front()->conn.peer_goaway_received());
            },
            options);
}

TEST(Http2ConnectionPoolTest, SettingsShrinkAndGrowRecomputeAvailableSlots) {
    run_case(
            [](PoolHarness &h) -> Task<void> {
                auto a = co_await h.acquire();
                auto b = co_await h.acquire();
                EXPECT_TRUE(a && b);
                if (!a || !b)
                    co_return;
                co_await h.settled(*a);
                co_await h.settings(*a, 1);
                auto blocked = co_await h.acquire(0ms);
                EXPECT_FALSE(blocked);
                b->reset();
                blocked = co_await h.acquire(0ms);
                EXPECT_FALSE(blocked);
                bool acquired = false;
                async::spawn(h.loop, [&]() -> DetachedTask {
                    auto c = co_await h.acquire();
                    EXPECT_TRUE(c);
                    acquired = true;
                });
                co_await async::sleep(5ms);
                EXPECT_FALSE(acquired);
                co_await h.settings(*a, 3);
                while (!acquired)
                    co_await async::sleep(1ms);
                EXPECT_EQ(h.dials, 1u);
            },
            single_connection(0));
}

TEST(Http2ConnectionPoolTest, PeerGoawayDrainsInFlightRequestAndAllowsReplacement) {
    run_case([](PoolHarness &h) -> Task<void> {
        auto lease = co_await h.acquire();
        EXPECT_TRUE(lease);
        if (!lease)
            co_return;
        co_await h.settled(*lease);
        auto *old = &lease->connection();
        mem::BufPool buffers;
        h.hold_responses = true;
        http::Http2PooledExchange ex(std::move(*lease), buffers);
        auto sent = co_await ex->send_request_header(
                {.method = http::HttpMethod::Get, .scheme = "http", .authority = "localhost", .path = "/"}, true, 1s);
        EXPECT_TRUE(sent);
        while (!h.requests)
            co_await async::sleep(1ms);
        h.servers.front()->conn.graceful_shutdown();
        while (!old->http2().peer_goaway_received())
            co_await async::sleep(1ms);
        auto next = co_await h.acquire();
        EXPECT_TRUE(next);
        if (next)
            EXPECT_NE(&next->connection(), old);
        h.hold_responses = false;
        auto response = co_await ex->read_header(1s);
        EXPECT_TRUE(response);
        ex.reset();
        EXPECT_EQ(h.dials, 2u);
    });
}

TEST(Http2ConnectionPoolTest, ClosedConnectionLivesUntilLastLeaseReturns) {
    run_case([](PoolHarness &h) -> Task<void> {
        auto lease = co_await h.acquire();
        EXPECT_TRUE(lease);
        if (!lease)
            co_return;
        co_await h.settled(*lease);
        h.servers.front()->conn.shutdown(IoErr::ConnReset);
        (void) co_await lease->connection().wait_closed();
        co_await async::yield();
        EXPECT_EQ(h.pool.connection_total(), 1u);
        EXPECT_EQ(h.pool.idle_total(), 0u);
        EXPECT_TRUE(lease->connection().close_gate().closed());
        EXPECT_EQ(lease->key(), h.key());
        lease->reset();
        EXPECT_EQ(h.pool.connection_total(), 1u); // Deferred, even outside the callback.
        co_await h.pool.join();
        EXPECT_EQ(h.pool.connection_total(), 0u);
    });
}

TEST(Http2ConnectionPoolTest, DialFailuresRetryWithoutBroadcastingError) {
    run_case([](PoolHarness &h) -> Task<void> {
        h.fail_dials = 2;
        unsigned completed = 0;
        for (int i = 0; i < 5; ++i) {
            async::spawn(h.loop, [&]() -> DetachedTask {
                auto lease = co_await h.acquire();
                EXPECT_TRUE(lease);
                ++completed;
            });
        }
        while (completed != 5)
            co_await async::sleep(1ms);
        EXPECT_EQ(h.dials, 3u);
        EXPECT_EQ(h.max_dials_active, 1u);
    });
}

TEST(Http2ConnectionPoolTest, PersistentDialFailureTimesOutWithBoundedRetries) {
    run_case([](PoolHarness &h) -> Task<void> {
        h.fail_dials = 1000;
        auto lease = co_await h.acquire(45ms);
        EXPECT_FALSE(lease);
        EXPECT_EQ(lease.error(), IoErr::TimedOut);
        EXPECT_GE(h.dials, 2u);
        EXPECT_LE(h.dials, 6u);
        co_await h.pool.join();
        EXPECT_EQ(h.pool.group_count(), 0u);
    });
}

TEST(Http2ConnectionPoolTest, DeadlineCancelsSuspendedConnector) {
    run_case([](PoolHarness &h) -> Task<void> {
        h.dial_delay = 1s;
        auto result = co_await h.acquire(10ms);
        EXPECT_FALSE(result);
        EXPECT_EQ(result.error(), IoErr::TimedOut);
        EXPECT_EQ(h.dials_active, 0u);
        co_await h.pool.join();
        EXPECT_EQ(h.pool.group_count(), 0u);
    });
}

TEST(Http2ConnectionPoolTest, ClearCancelsDialAndWaitersAndCanBeReused) {
    run_case([](PoolHarness &h) -> Task<void> {
        h.dial_delay = 1s;
        unsigned done = 0;
        for (int i = 0; i < 3; ++i) {
            async::spawn(h.loop, [&]() -> DetachedTask {
                auto result = co_await h.acquire();
                EXPECT_FALSE(result);
                EXPECT_EQ(result.error(), IoErr::Canceled);
                ++done;
            });
        }
        while (!h.dials_active)
            co_await async::sleep(1ms);
        h.pool.clear();
        co_await h.pool.join();
        EXPECT_EQ(done, 3u);
        EXPECT_EQ(h.dials_active, 0u);
        h.dial_delay = 0ms;
        auto lease = co_await h.acquire();
        EXPECT_TRUE(lease);
    });
}

TEST(Http2ConnectionPoolTest, ShutdownWithLiveLeaseCancelsWaitersAndJoinsAfterRelease) {
    run_case(
            [](PoolHarness &h) -> Task<void> {
                auto lease = co_await h.acquire();
                EXPECT_TRUE(lease);
                if (!lease)
                    co_return;
                bool done = false;
                async::spawn(h.loop, [&]() -> DetachedTask {
                    auto result = co_await h.acquire();
                    EXPECT_FALSE(result);
                    EXPECT_EQ(result.error(), IoErr::Canceled);
                    done = true;
                });
                co_await async::sleep(2ms);
                h.pool.shutdown();
                while (!done)
                    co_await async::sleep(1ms);
                EXPECT_EQ(h.pool.connection_total(), 1u);
                auto rejected = co_await h.acquire(0ms);
                EXPECT_FALSE(rejected);
                EXPECT_EQ(rejected.error(), IoErr::Canceled);
                lease->reset();
                co_await h.pool.join();
            },
            single_connection());
}

TEST(Http2ConnectionPoolTest, LifetimeLimitRetiresAfterFinalLease) {
    auto options = single_connection(2);
    options.max_streams_lifetime = 1;
    run_case(
            [](PoolHarness &h) -> Task<void> {
                auto a = co_await h.acquire();
                auto b = co_await h.acquire();
                EXPECT_TRUE(a && b);
                if (!a || !b)
                    co_return;
                a->reset();
                auto poll = co_await h.acquire(0ms);
                EXPECT_FALSE(poll);
                EXPECT_EQ(h.pool.connection_total(), 1u);
                b->reset();
                co_await h.pool.join();
                auto next = co_await h.acquire();
                EXPECT_TRUE(next);
                EXPECT_EQ(h.dials, 2u);
            },
            options);
}

TEST(Http2ConnectionPoolTest, ZeroIdleBudgetStillAllowsAcquireAndReusesEntryStorage) {
    auto options = single_connection();
    options.max_idle_total = 0;
    run_case(
            [](PoolHarness &h) -> Task<void> {
                http::Http2ClientConnection *storage = nullptr;
                for (int i = 0; i < 100; ++i) {
                    auto lease = co_await h.acquire();
                    EXPECT_TRUE(lease);
                    if (!lease)
                        co_return;
                    if (storage)
                        EXPECT_EQ(&lease->connection(), storage);
                    storage = &lease->connection();
                    lease->reset();
                    co_await h.pool.join();
                    EXPECT_EQ(h.pool.group_count(), 0u);
                }
                EXPECT_EQ(h.dials, 100u);
            },
            options);
}

TEST(Http2ConnectionPoolTest, GlobalLimitReleaseWakesAnotherGroup) {
    auto options = single_connection();
    options.max_connections_total = 1;
    options.max_idle_total = 0;
    run_case(
            [](PoolHarness &h) -> Task<void> {
                auto first = co_await h.acquire();
                EXPECT_TRUE(first);
                if (!first)
                    co_return;
                bool done = false;
                async::spawn(h.loop, [&]() -> DetachedTask {
                    auto second = co_await h.pool.acquire(h.key(1), h.connector(), 1s);
                    EXPECT_TRUE(second);
                    if (second)
                        EXPECT_EQ(second->key(), h.key(1));
                    done = true;
                });
                co_await async::sleep(5ms);
                EXPECT_FALSE(done);
                first->reset();
                while (!done)
                    co_await async::sleep(1ms);
                EXPECT_EQ(h.dials, 2u);
            },
            options);
}

TEST(Http2ConnectionPoolTest, DestroyingAcquireCoroutineCancelsWaiterAndDial) {
    run_case(
            [](PoolHarness &h) -> Task<void> {
                h.dial_delay = 1s;
                auto result = co_await async::timeout_for([&]() { return h.acquire().select(); }, 5ms);
                EXPECT_FALSE(result);
                EXPECT_EQ(result.error(), IoErr::TimedOut);
                co_await h.pool.join();
                EXPECT_EQ(h.dials_active, 0u);
                EXPECT_EQ(h.pool.group_count(), 0u);
                h.dial_delay = 0ms;
                auto lease = co_await h.acquire();
                EXPECT_TRUE(lease);
                if (!lease)
                    co_return;
                result = co_await async::timeout_for([&]() { return h.acquire().select(); }, 5ms);
                EXPECT_FALSE(result);
                EXPECT_EQ(result.error(), IoErr::TimedOut);
                lease->reset();
                auto next = co_await h.acquire(0ms);
                EXPECT_TRUE(next);
            },
            single_connection());
}

TEST(Http2ConnectionPoolTest, PooledExchangeMoveAssignmentReleasesOldSlotFirst) {
    auto options = single_connection(2);
    run_case(
            [](PoolHarness &h) -> Task<void> {
                auto a = co_await h.acquire();
                auto b = co_await h.acquire();
                EXPECT_TRUE(a && b);
                if (!a || !b)
                    co_return;
                mem::BufPool buffers;
                http::Http2PooledExchange left(std::move(*a), buffers), right(std::move(*b), buffers);
                left = std::move(right);
                EXPECT_TRUE(left.valid());
                EXPECT_FALSE(right.valid());
                auto free = co_await h.acquire(0ms);
                EXPECT_TRUE(free);
                auto full = co_await h.acquire(0ms);
                EXPECT_FALSE(full);
                left.reset();
                free->reset();
                EXPECT_EQ(h.pool.idle_total(), 1u);
            },
            options);
}
TEST(Http2ConnectionPoolTest, IdleBudgetEvictsOldestAndUnsaturatedReleaseKeepsOrder) {
    auto options = single_connection(2);
    options.max_connections_per_group = 3;
    options.max_idle_total = 1;
    run_case(
            [](PoolHarness &h) -> Task<void> {
                auto a1 = co_await h.acquire();
                auto a2 = co_await h.acquire();
                auto b1 = co_await h.acquire();
                auto b2 = co_await h.acquire();
                EXPECT_TRUE(a1 && a2 && b1 && b2);
                if (!a1 || !a2 || !b1 || !b2)
                    co_return;
                co_await h.settled(*a1);
                co_await h.settled(*b1);
                auto *a = &a1->connection();
                auto *b = &b1->connection();
                a1->reset();
                b1->reset();
                a2->reset(); // B remains the head; A was already ready.
                auto next = co_await h.acquire();
                EXPECT_TRUE(next);
                if (next)
                    EXPECT_EQ(&next->connection(), b);
                EXPECT_NE(a, b);
                b2->reset();
                next->reset(); // A is the oldest idle connection.
                EXPECT_EQ(h.pool.idle_total(), 1u);
                while (h.pool.connection_total() != 1)
                    co_await async::sleep(1ms);
                auto survivor = co_await h.acquire();
                EXPECT_TRUE(survivor);
                if (survivor)
                    EXPECT_EQ(&survivor->connection(), b);
            },
            options);
}

TEST(Http2ConnectionPoolTest, ParallelDialOptionHonorsConfiguredLimit) {
    auto options = single_connection();
    options.max_connections_per_group = 4;
    options.max_concurrent_dials_per_group = 2;
    run_case(
            [](PoolHarness &h) -> Task<void> {
                h.dial_delay = 10ms;
                unsigned acquired = 0, done = 0;
                bool release = false;
                for (int i = 0; i < 4; ++i) {
                    async::spawn(h.loop, [&]() -> DetachedTask {
                        auto lease = co_await h.acquire();
                        EXPECT_TRUE(lease);
                        ++acquired;
                        while (!release)
                            co_await async::sleep(1ms);
                        ++done;
                    });
                }
                while (acquired != 4)
                    co_await async::sleep(1ms);
                EXPECT_EQ(h.dials, 4u);
                EXPECT_EQ(h.max_dials_active, 2u);
                release = true;
                while (done != 4)
                    co_await async::sleep(1ms);
            },
            options);
}

TEST(Http2ConnectionPoolTest, PollNeverDialsAndPreSettingsLimitIsConservative) {
    auto options = single_connection(0);
    options.pre_settings_max_streams = 1;
    run_case(
            [](PoolHarness &h) -> Task<void> {
                auto poll = co_await h.acquire(0ms);
                EXPECT_FALSE(poll);
                EXPECT_EQ(poll.error(), IoErr::Busy);
                EXPECT_EQ(h.dials, 0u);
                EXPECT_EQ(h.pool.group_count(), 0u);
                auto lease = co_await h.acquire();
                EXPECT_TRUE(lease);
                if (!lease)
                    co_return;
                if (!lease->connection().http2().peer_settings_received()) {
                    auto before = co_await h.acquire(0ms);
                    EXPECT_FALSE(before);
                    EXPECT_EQ(before.error(), IoErr::Busy);
                }
                co_await h.settled(*lease);
                auto after = co_await h.acquire(0ms);
                EXPECT_TRUE(after);
            },
            options);
}

TEST(Http2ConnectionPoolTest, CountObserverReportsReadyTransitionsAndFinalZero) {
    struct Counts {
        std::size_t total = 0, ready = 0;
        unsigned calls = 0;
    };
    run_case(
            [](PoolHarness &h) -> Task<void> {
                Counts counts;
                h.pool.set_conn_count_changed_callback(
                        [](void *ctx, const HttpConnectionGroupKey &, std::size_t total, std::size_t ready) noexcept {
                            auto &counts = *static_cast<Counts *>(ctx);
                            counts.total = total;
                            counts.ready = ready;
                            ++counts.calls;
                            EXPECT_LE(ready, total);
                        },
                        &counts);
                auto lease = co_await h.acquire();
                EXPECT_TRUE(lease);
                EXPECT_EQ(counts.total, 1u);
                EXPECT_EQ(counts.ready, 0u);
                if (lease)
                    lease->reset();
                EXPECT_EQ(counts.ready, 1u);
                h.pool.clear();
                co_await h.pool.join();
                EXPECT_EQ(counts.total, 0u);
                EXPECT_EQ(counts.ready, 0u);
                EXPECT_GE(counts.calls, 5u);
                h.pool.clear_conn_count_changed_callback();
            },
            single_connection());
}

TEST(Http2ConnectionPoolTest, LeaseKeysSurviveBucketIndexGrowthAndBackwardShift) {
    auto options = single_connection();
    options.max_connections_total = 48;
    run_case(
            [](PoolHarness &h) -> Task<void> {
                std::vector<Lease> leases;
                for (std::uint16_t i = 1; i <= 40; ++i) {
                    auto lease = co_await h.pool.acquire(h.key(i), h.connector(), 1s);
                    EXPECT_TRUE(lease);
                    if (!lease)
                        break;
                    leases.push_back(std::move(*lease));
                }
                for (std::size_t i = 0; i < leases.size(); ++i)
                    EXPECT_EQ(leases[i].key(), h.key(i + 1));
                h.pool.clear();
                for (std::size_t i = 0; i < leases.size(); i += 2)
                    leases[i].reset();
                co_await async::sleep(5ms);
                for (std::size_t i = 1; i < leases.size(); i += 2)
                    EXPECT_EQ(leases[i].key(), h.key(i + 1));
            },
            options);
}

TEST(LocalHttp2ConnectionPoolSetTest, ShutdownWaitsForRealConnectionLeaseAndDeferredDestruction) {
    run_case([](PoolHarness &h) -> Task<void> {
        http::LocalHttp2ConnectionPoolSet set(*h.loop.group());
        EXPECT_TRUE(set.init());
        auto lease = co_await set.acquire(h.key(), h.connector(), 1s);
        EXPECT_TRUE(lease);
        if (!lease)
            co_return;
        bool released = false;
        async::spawn(h.loop, [&]() -> DetachedTask {
            co_await async::sleep(10ms);
            EXPECT_EQ(set.connection_total(), 1u);
            lease->reset();
            released = true;
        });
        co_await set.shutdown_async();
        EXPECT_TRUE(released);
        EXPECT_EQ(set.connection_total(), 0u);
        EXPECT_EQ(set.group_count(), 0u);
    });
}
TEST(Http2ConnectionPoolTest, AbandonedPooledExchangeCancelsStreamBeforeReturningSlot) {
    run_case(
            [](PoolHarness &h) -> Task<void> {
                auto lease = co_await h.acquire();
                EXPECT_TRUE(lease);
                if (!lease)
                    co_return;
                co_await h.settled(*lease);
                auto *connection = &lease->connection();
                mem::BufPool buffers;
                h.hold_responses = true;
                {
                    http::Http2PooledExchange ex(std::move(*lease), buffers);
                    auto sent = co_await ex->send_request_header(
                            {.method = http::HttpMethod::Get, .scheme = "http", .authority = "localhost", .path = "/"},
                            true, 1s);
                    EXPECT_TRUE(sent);
                    while (!h.requests)
                        co_await async::sleep(1ms);
                    EXPECT_EQ(connection->http2().local_active_stream_count(), 1u);
                }
                EXPECT_EQ(connection->http2().local_active_stream_count(), 0u);
                auto next = co_await h.acquire(0ms);
                EXPECT_TRUE(next);
                if (next)
                    EXPECT_EQ(&next->connection(), connection);
                h.hold_responses = false;
            },
            single_connection());
}
} // namespace
