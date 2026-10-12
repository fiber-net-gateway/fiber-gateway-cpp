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
                                                       const http::HttpConnectionGroupKey & /*unused*/) noexcept {
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
    EXPECT_EQ(set.size(), 3U);
    EXPECT_EQ(set.options().initial_group_capacity, 7U);
}

TEST(LocalHttp2ConnectionPoolSetTest, ShutdownCancelsEveryShardAndConcurrentCallersJoinDrain) {
    // NOLINTBEGIN(bugprone-unchecked-optional-access) ASSERT guard is opaque to the checker
    event::EventLoopGroup group(2);
    http::LocalHttp2ConnectionPoolSet set(group);
    ASSERT_TRUE(set.init());
    std::atomic<unsigned> started{0}, canceled{0}, completed{0};
    std::array<PendingConnector, 2> connectors{
            {{.expected_loop = &group.at(0), .started = &started, .canceled = &canceled},
             {.expected_loop = &group.at(1), .started = &started, .canceled = &canceled}}};
    std::array<std::promise<void>, 2> done;
    auto f0 = done[0].get_future();
    auto f1 = done[1].get_future();
    const auto key = *http::HttpConnectionGroupKey::make("127.0.0.1", 80, http::HttpConnectionGroupKey::Scheme::Http);
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
            EXPECT_EQ(canceled.load(), 2U);
            EXPECT_EQ(completed.load(), 2U);
            EXPECT_EQ(set.connection_total(), 0U);
            EXPECT_EQ(set.group_count(), 0U);
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
    // NOLINTEND(bugprone-unchecked-optional-access)
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
        const auto key =
                *http::HttpConnectionGroupKey::make("127.0.0.1", 80, http::HttpConnectionGroupKey::Scheme::Http);
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
