#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/http/LocalHttp2ConnectionPoolSet.h>
#include <future>

namespace {
using namespace std::chrono_literals;
using namespace fiber;

struct PendingConnector {
    event::EventLoop *expected_loop;
    std::atomic<unsigned> *started;
    std::atomic<unsigned> *canceled;
    static async::Task<common::IoResult<void>> connect(void *ctx, http::Http2ClientConnection &conn,
                                                       const http::HttpConnectionGroupKey &) noexcept {
        auto &self = *static_cast<PendingConnector *>(ctx);
        EXPECT_EQ(&conn.loop(), self.expected_loop);
        EXPECT_EQ(&event::EventLoop::current(), self.expected_loop);
        struct Guard {
            PendingConnector &self;
            ~Guard() { self.canceled->fetch_add(1); }
        } guard{self};
        self.started->fetch_add(1);
        co_await async::sleep(10s);
        co_return std::unexpected(common::IoErr::ConnRefused);
    }
};

TEST(LocalHttp2ConnectionPoolSetTest, InitializesOneCorePerLoop) {
    event::EventLoopGroup group(3);
    http::LocalHttp2ConnectionPoolSet::Options options;
    options.initial_group_capacity = 7;
    http::LocalHttp2ConnectionPoolSet set(group, options);
    EXPECT_TRUE(set.init());
    EXPECT_EQ(set.size(), 3u);
    EXPECT_EQ(set.options().initial_group_capacity, 7u);
}

TEST(LocalHttp2ConnectionPoolSetTest, ShutdownCancelsEveryShardAndConcurrentCallersJoinDrain) {
    event::EventLoopGroup group(2);
    http::LocalHttp2ConnectionPoolSet set(group);
    ASSERT_TRUE(set.init());
    std::atomic<unsigned> started{0}, canceled{0}, completed{0};
    std::array<PendingConnector, 2> connectors{
            {{&group.at(0), &started, &canceled}, {&group.at(1), &started, &canceled}}};
    std::array<std::promise<void>, 2> done;
    auto f0 = done[0].get_future();
    auto f1 = done[1].get_future();
    const auto key = http::HttpConnectionGroupKey::from_ip(net::IpAddress::loopback_v4(), 80,
                                                           http::HttpConnectionGroupKey::Scheme::Http);
    group.start();
    for (unsigned i = 0; i < 2; ++i) {
        async::spawn(group.at(i), [&, i]() -> async::DetachedTask {
            EXPECT_EQ(&set.loop(), &group.at(i));
            auto lease = co_await set.acquire(key, {&PendingConnector::connect, &connectors[i]}, 5s);
            EXPECT_FALSE(lease);
            EXPECT_EQ(lease.error(), common::IoErr::Canceled);
            completed.fetch_add(1);
        });
        async::spawn(group.at(i), [&, i]() -> async::DetachedTask {
            while (started.load() != 2)
                co_await async::sleep(1ms);
            co_await set.shutdown_async();
            EXPECT_EQ(canceled.load(), 2u);
            EXPECT_EQ(completed.load(), 2u);
            EXPECT_EQ(set.connection_total(), 0u);
            EXPECT_EQ(set.group_count(), 0u);
            auto lease = co_await set.acquire(key, {}, 0ms);
            EXPECT_FALSE(lease);
            EXPECT_EQ(lease.error(), common::IoErr::Canceled);
            done[i].set_value();
        });
    }
    ASSERT_EQ(f0.wait_for(3s), std::future_status::ready);
    ASSERT_EQ(f1.wait_for(3s), std::future_status::ready);
    group.stop();
    group.join();
}

TEST(LocalHttp2ConnectionPoolSetTest, ClearAcrossShardsAllowsSubsequentAcquire) {
    event::EventLoopGroup group(2);
    http::LocalHttp2ConnectionPoolSet set(group);
    ASSERT_TRUE(set.init());
    std::promise<void> done;
    auto future = done.get_future();
    group.start();
    async::spawn(group.at(0), [&]() -> async::DetachedTask {
        co_await set.clear_async();
        const auto key = http::HttpConnectionGroupKey::from_ip(net::IpAddress::loopback_v4(), 80,
                                                               http::HttpConnectionGroupKey::Scheme::Http);
        auto poll = co_await set.acquire(key, {}, 0ms);
        EXPECT_FALSE(poll);
        EXPECT_EQ(poll.error(), common::IoErr::Busy);
        co_await set.shutdown_async();
        done.set_value();
    });
    ASSERT_EQ(future.wait_for(3s), std::future_status::ready);
    group.stop();
    group.join();
}
} // namespace
