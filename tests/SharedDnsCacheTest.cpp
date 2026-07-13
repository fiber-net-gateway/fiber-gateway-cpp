#include <gtest/gtest.h>

#include <chrono>
#include <future>

#include "async/Spawn.h"
#include "dns/DnsCache.h"
#include "event/EventLoopGroup.h"

namespace {

using DetachedTask = fiber::async::DetachedTask;
using fiber::common::IoErr;
using fiber::dns::RecordClass;
using fiber::dns::SharedDnsCache;
using fiber::net::IpAddress;

struct LookupResult {
    IoErr err = IoErr::Invalid;
    bool found = false;
    bool a_present = false;
    bool a_negative = false;
    IpAddress address{};
};

struct ExpiredCleanupResult {
    IoErr err = IoErr::Invalid;
    std::size_t count_before = 0;
    std::size_t count_after = 0;
    bool found = true;
};

struct EvictionResult {
    IoErr err = IoErr::Invalid;
    bool first_found = true;
    bool second_found = false;
    bool third_found = false;
};

DetachedTask write_a_record(SharedDnsCache *cache, std::promise<IoErr> *done) {
    auto now = std::chrono::steady_clock::now();
    IpAddress address = IpAddress::v4({9, 9, 9, 9});
    done->set_value(co_await cache->upsert_a("shared.example", static_cast<std::uint16_t>(RecordClass::IN), &address, 1,
                                             now + std::chrono::seconds(30)));
    co_return;
}

DetachedTask read_name(SharedDnsCache *cache, std::promise<LookupResult> *done) {
    LookupResult result;
    fiber::dns::NameSnapshot snapshot;
    if (!snapshot.init()) {
        done->set_value(result);
        co_return;
    }

    auto now = std::chrono::steady_clock::now();
    result.err =
            co_await cache->lookup_name("shared.example", static_cast<std::uint16_t>(RecordClass::IN), now, snapshot);
    result.found = snapshot.found();
    result.a_present = snapshot.a().present;
    result.a_negative = snapshot.a().negative;
    if (snapshot.a().present && snapshot.a().count != 0) {
        result.address = snapshot.a().records[0];
    }
    done->set_value(result);
    co_return;
}

DetachedTask exercise_expired_cleanup(SharedDnsCache *cache, std::promise<ExpiredCleanupResult> *done) {
    ExpiredCleanupResult result;
    fiber::dns::NameSnapshot snapshot;
    if (!snapshot.init()) {
        done->set_value(result);
        co_return;
    }

    auto now = fiber::event::EventLoop::current().now();
    IpAddress address = IpAddress::v4({10, 0, 0, 2});
    result.err = co_await cache->upsert_a("expired.shared", static_cast<std::uint16_t>(RecordClass::IN), &address, 1,
                                          now - std::chrono::seconds(1));
    if (result.err != IoErr::None) {
        done->set_value(result);
        co_return;
    }
    result.count_before = co_await cache->entry_count();
    result.err =
            co_await cache->lookup_name("expired.shared", static_cast<std::uint16_t>(RecordClass::IN), now, snapshot);
    result.found = snapshot.found();
    result.count_after = co_await cache->entry_count();
    done->set_value(result);
    co_return;
}

DetachedTask exercise_shared_eviction(SharedDnsCache *cache, std::promise<EvictionResult> *done) {
    EvictionResult result;
    fiber::dns::NameSnapshot snapshot;
    if (!snapshot.init()) {
        done->set_value(result);
        co_return;
    }

    auto now = fiber::event::EventLoop::current().now();
    IpAddress first = IpAddress::v4({1, 1, 1, 1});
    IpAddress second = IpAddress::v4({2, 2, 2, 2});
    IpAddress third = IpAddress::v4({3, 3, 3, 3});
    result.err = co_await cache->upsert_a("first.shared", static_cast<std::uint16_t>(RecordClass::IN), &first, 1,
                                          now + std::chrono::seconds(60));
    if (result.err == IoErr::None) {
        result.err = co_await cache->upsert_a("second.shared", static_cast<std::uint16_t>(RecordClass::IN), &second, 1,
                                              now + std::chrono::seconds(60));
    }
    if (result.err == IoErr::None) {
        result.err = co_await cache->lookup_name("second.shared", static_cast<std::uint16_t>(RecordClass::IN), now,
                                                 snapshot);
    }
    if (result.err == IoErr::None) {
        result.err = co_await cache->upsert_a("third.shared", static_cast<std::uint16_t>(RecordClass::IN), &third, 1,
                                              now + std::chrono::seconds(60));
    }
    if (result.err != IoErr::None) {
        done->set_value(result);
        co_return;
    }

    result.err =
            co_await cache->lookup_name("first.shared", static_cast<std::uint16_t>(RecordClass::IN), now, snapshot);
    result.first_found = snapshot.found();
    if (result.err == IoErr::None) {
        result.err = co_await cache->lookup_name("second.shared", static_cast<std::uint16_t>(RecordClass::IN), now,
                                                 snapshot);
        result.second_found = snapshot.found();
    }
    if (result.err == IoErr::None) {
        result.err =
                co_await cache->lookup_name("third.shared", static_cast<std::uint16_t>(RecordClass::IN), now, snapshot);
        result.third_found = snapshot.found();
    }
    done->set_value(result);
    co_return;
}

} // namespace

TEST(SharedDnsCacheTest, SupportsCrossLoopReadAndWrite) {
    fiber::event::EventLoopGroup group(2);
    SharedDnsCache cache;
    ASSERT_TRUE(cache.init());

    std::promise<IoErr> write_done;
    auto write_future = write_done.get_future();
    std::promise<LookupResult> read_done;
    auto read_future = read_done.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&cache, &write_done]() { return write_a_record(&cache, &write_done); });

    if (write_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "shared cache write did not complete in time";
        return;
    }
    ASSERT_EQ(write_future.get(), IoErr::None);

    fiber::async::spawn(group.at(1), [&cache, &read_done]() { return read_name(&cache, &read_done); });

    if (read_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "shared cache read did not complete in time";
        return;
    }

    LookupResult result = read_future.get();
    EXPECT_EQ(result.err, IoErr::None);
    EXPECT_TRUE(result.found);
    EXPECT_TRUE(result.a_present);
    EXPECT_FALSE(result.a_negative);
    EXPECT_TRUE(result.address.is_v4());
    EXPECT_EQ(result.address.v4_bytes(), IpAddress::v4({9, 9, 9, 9}).v4_bytes());

    group.stop();
    group.join();
}

TEST(SharedDnsCacheTest, LookupReclaimsExpiredEntry) {
    fiber::event::EventLoopGroup group(1);
    SharedDnsCache cache;
    ASSERT_TRUE(cache.init());

    std::promise<ExpiredCleanupResult> done;
    auto future = done.get_future();
    group.start();
    fiber::async::spawn(group.at(0), [&cache, &done]() { return exercise_expired_cleanup(&cache, &done); });

    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "shared cache expired lookup did not complete in time";
        return;
    }
    ExpiredCleanupResult result = future.get();
    group.stop();
    group.join();

    EXPECT_EQ(result.err, IoErr::None);
    EXPECT_EQ(result.count_before, 1u);
    EXPECT_FALSE(result.found);
    EXPECT_EQ(result.count_after, 0u);
}

TEST(SharedDnsCacheTest, LookupRefreshesEvictionAge) {
    fiber::event::EventLoopGroup group(1);
    SharedDnsCache cache;
    SharedDnsCache::Options options;
    options.max_entries = 2;
    options.max_bytes = 4096;
    options.eviction_sample = 2;
    ASSERT_TRUE(cache.init(options));

    std::promise<EvictionResult> done;
    auto future = done.get_future();
    group.start();
    fiber::async::spawn(group.at(0), [&cache, &done]() { return exercise_shared_eviction(&cache, &done); });

    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "shared cache eviction exercise did not complete in time";
        return;
    }
    EvictionResult result = future.get();
    group.stop();
    group.join();

    EXPECT_EQ(result.err, IoErr::None);
    EXPECT_FALSE(result.first_found);
    EXPECT_TRUE(result.second_found);
    EXPECT_TRUE(result.third_found);
}
