#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <thread>

#include <fiber/async/Spawn.h>
#include <fiber/dns/DnsCache2.h>
#include <fiber/event/EventLoopGroup.h>

namespace {

using DetachedTask = fiber::async::DetachedTask;
using fiber::common::IoErr;
using fiber::dns::DnsCacheKey;
using fiber::dns::DnsCacheOut;
using fiber::dns::DnsCacheOutKind;
using fiber::dns::SharedDnsCache2;
using fiber::net::IpAddress;

DnsCacheKey shared_key() {
    constexpr std::string_view name = "shared-cache2.example";
    return {name, fiber::dns::dns_cache_hash(name)};
}

DetachedTask write_record(SharedDnsCache2 *cache, std::promise<IoErr> *done) {
    const auto now = fiber::event::EventLoop::current().now();
    const IpAddress address = IpAddress::v4({9, 9, 9, 9});
    done->set_value(cache->upsert_address_set(shared_key(), now, fiber::net::IpFamily::V4, &address, 1,
                                              now + std::chrono::seconds(30)));
    co_return;
}

struct LookupResult {
    IoErr err = IoErr::Invalid;
    DnsCacheOut out{};
};

DetachedTask read_record(SharedDnsCache2 *cache, DnsCacheKey key, std::promise<LookupResult> *done) {
    LookupResult result;
    result.err = cache->lookup(key, fiber::event::EventLoop::current().now(), result.out);
    done->set_value(result);
    co_return;
}

DetachedTask write_short_lived_record(SharedDnsCache2 *cache, std::promise<IoErr> *done) {
    const auto now = fiber::event::EventLoop::current().now();
    const IpAddress address = IpAddress::v4({1, 1, 1, 1});
    done->set_value(cache->upsert_address_set(shared_key(), now, fiber::net::IpFamily::V4, &address, 1,
                                              now + std::chrono::milliseconds(25)));
    co_return;
}

DetachedTask read_entry_count(SharedDnsCache2 *cache, std::promise<std::size_t> *done) {
    done->set_value(cache->entry_count());
    co_return;
}

DetachedTask shutdown_cache(SharedDnsCache2 *cache, std::promise<void> *done) {
    cache->shutdown();
    done->set_value();
    co_return;
}

} // namespace

TEST(SharedDnsCache2Test, CopiesResultsAcrossEventLoops) {
    fiber::event::EventLoopGroup group(2);
    SharedDnsCache2 cache;
    ASSERT_TRUE(cache.init(group.at(0)));

    std::promise<IoErr> write_done;
    auto write_future = write_done.get_future();
    std::promise<LookupResult> read_done;
    auto read_future = read_done.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() { return write_record(&cache, &write_done); });
    ASSERT_EQ(write_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ASSERT_EQ(write_future.get(), IoErr::None);

    fiber::async::spawn(group.at(1), [&]() { return read_record(&cache, shared_key(), &read_done); });
    ASSERT_EQ(read_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const LookupResult result = read_future.get();

    std::promise<void> shutdown_done;
    auto shutdown_future = shutdown_done.get_future();
    fiber::async::spawn(group.at(0), [&]() { return shutdown_cache(&cache, &shutdown_done); });
    ASSERT_EQ(shutdown_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);

    group.stop();
    group.join();

    ASSERT_EQ(result.err, IoErr::None);
    ASSERT_EQ(result.out.kind, DnsCacheOutKind::Addresses);
    ASSERT_EQ(result.out.value.addresses.address_set.count, 1);
    EXPECT_EQ(result.out.value.addresses.address_set.records[0], IpAddress::v4({9, 9, 9, 9}));
}

TEST(SharedDnsCache2Test, BoundEventLoopRemovesExpiredEntries) {
    fiber::event::EventLoopGroup group(1);
    SharedDnsCache2 cache;
    ASSERT_TRUE(cache.init(group.at(0)));

    std::promise<IoErr> write_done;
    auto write_future = write_done.get_future();
    group.start();
    fiber::async::spawn(group.at(0), [&]() { return write_short_lived_record(&cache, &write_done); });
    ASSERT_EQ(write_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ASSERT_EQ(write_future.get(), IoErr::None);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::promise<std::size_t> count_done;
    auto count_future = count_done.get_future();
    fiber::async::spawn(group.at(0), [&]() { return read_entry_count(&cache, &count_done); });
    ASSERT_EQ(count_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(count_future.get(), 0u);

    std::promise<void> shutdown_done;
    auto shutdown_future = shutdown_done.get_future();
    fiber::async::spawn(group.at(0), [&]() { return shutdown_cache(&cache, &shutdown_done); });
    ASSERT_EQ(shutdown_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);

    group.stop();
    group.join();
}

TEST(SharedDnsCache2Test, UpsertWorksWithoutCurrentEventLoop) {
    fiber::event::EventLoopGroup group(1);
    SharedDnsCache2 cache;
    ASSERT_TRUE(cache.init(group.at(0)));
    group.start();

    constexpr std::string_view address_name = "no-loop-address.example";
    constexpr std::string_view cname_name = "no-loop-cname.example";
    constexpr std::string_view nxdomain_name = "no-loop-nxdomain.example";
    const DnsCacheKey address_key{address_name, fiber::dns::dns_cache_hash(address_name)};
    const DnsCacheKey cname_key{cname_name, fiber::dns::dns_cache_hash(cname_name)};
    const DnsCacheKey nxdomain_key{nxdomain_name, fiber::dns::dns_cache_hash(nxdomain_name)};
    const auto now = std::chrono::steady_clock::now();
    const IpAddress address = IpAddress::v4({8, 8, 4, 4});
    ASSERT_EQ(cache.upsert_address_set(address_key, now, fiber::net::IpFamily::V4, &address, 1,
                                       now + std::chrono::seconds(30)),
              IoErr::None);
    ASSERT_EQ(cache.upsert_cname(cname_key, now, "target.example", now + std::chrono::seconds(30)), IoErr::None);
    ASSERT_EQ(cache.upsert_nxdomain(nxdomain_key, now, now + std::chrono::seconds(30)), IoErr::None);

    std::promise<LookupResult> address_done;
    std::promise<LookupResult> cname_done;
    std::promise<LookupResult> nxdomain_done;
    auto address_future = address_done.get_future();
    auto cname_future = cname_done.get_future();
    auto nxdomain_future = nxdomain_done.get_future();
    fiber::async::spawn(group.at(0), [&]() { return read_record(&cache, address_key, &address_done); });
    fiber::async::spawn(group.at(0), [&]() { return read_record(&cache, cname_key, &cname_done); });
    fiber::async::spawn(group.at(0), [&]() { return read_record(&cache, nxdomain_key, &nxdomain_done); });
    ASSERT_EQ(address_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ASSERT_EQ(cname_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ASSERT_EQ(nxdomain_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);

    const LookupResult address_result = address_future.get();
    const LookupResult cname_result = cname_future.get();
    const LookupResult nxdomain_result = nxdomain_future.get();
    EXPECT_EQ(address_result.err, IoErr::None);
    EXPECT_EQ(address_result.out.kind, DnsCacheOutKind::Addresses);
    EXPECT_EQ(cname_result.err, IoErr::None);
    EXPECT_EQ(cname_result.out.kind, DnsCacheOutKind::Cname);
    EXPECT_EQ(nxdomain_result.err, IoErr::None);
    EXPECT_EQ(nxdomain_result.out.kind, DnsCacheOutKind::NxDomain);

    std::promise<void> shutdown_done;
    auto shutdown_future = shutdown_done.get_future();
    fiber::async::spawn(group.at(0), [&]() { return shutdown_cache(&cache, &shutdown_done); });
    ASSERT_EQ(shutdown_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    group.stop();
    group.join();
}

TEST(SharedDnsCache2Test, CallerSuppliedNowDrivesExpiry) {
    fiber::event::EventLoopGroup group(1);
    SharedDnsCache2 cache;
    ASSERT_TRUE(cache.init(group.at(0)));
    group.start();

    const auto now = std::chrono::steady_clock::now();
    const IpAddress address = IpAddress::v4({8, 8, 8, 8});
    ASSERT_EQ(cache.upsert_address_set(shared_key(), now, fiber::net::IpFamily::V4, &address, 1,
                                       now - std::chrono::milliseconds(1)),
              IoErr::None);

    DnsCacheOut out{};
    ASSERT_EQ(cache.lookup(shared_key(), now + std::chrono::milliseconds(1), out), IoErr::None);
    EXPECT_EQ(out.kind, DnsCacheOutKind::Miss);
    EXPECT_EQ(cache.entry_count(), 0u);

    std::promise<void> shutdown_done;
    auto shutdown_future = shutdown_done.get_future();
    fiber::async::spawn(group.at(0), [&]() { return shutdown_cache(&cache, &shutdown_done); });
    ASSERT_EQ(shutdown_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    group.stop();
    group.join();
}
