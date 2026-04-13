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
