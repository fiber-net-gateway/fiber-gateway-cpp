#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <string>

#include "dns/DnsCache.h"

namespace {

using fiber::common::IoErr;
using fiber::dns::DnsCache;
using fiber::dns::NameSnapshot;
using fiber::dns::RecordClass;
using fiber::dns::RecordType;
using fiber::net::IpAddress;

bool same_ip(const IpAddress &left, const IpAddress &right) {
    if (left.family() != right.family()) {
        return false;
    }
    if (left.is_v4()) {
        return left.v4_bytes() == right.v4_bytes();
    }
    return left.v6_bytes() == right.v6_bytes() && left.scope_id() == right.scope_id();
}

} // namespace

TEST(DnsCacheTest, LookupReturnsAllCachedSlotsForName) {
    DnsCache cache;
    ASSERT_TRUE(cache.init());

    NameSnapshot snapshot;
    ASSERT_TRUE(snapshot.init());

    auto now = std::chrono::steady_clock::now();
    IpAddress a = IpAddress::v4({1, 1, 1, 1});
    std::array<std::uint8_t, 16> v6_bytes{};
    v6_bytes[0] = 0x20;
    v6_bytes[1] = 0x01;
    v6_bytes[2] = 0x0d;
    v6_bytes[3] = 0xb8;
    v6_bytes[15] = 1;
    IpAddress aaaa = IpAddress::v6(v6_bytes);

    ASSERT_EQ(cache.upsert_a("Example.COM.", static_cast<std::uint16_t>(RecordClass::IN), &a, 1,
                             now + std::chrono::seconds(60)),
              IoErr::None);
    ASSERT_EQ(cache.upsert_aaaa("example.com", static_cast<std::uint16_t>(RecordClass::IN), &aaaa, 1,
                                now + std::chrono::seconds(90)),
              IoErr::None);
    ASSERT_EQ(cache.upsert_cname("example.com", static_cast<std::uint16_t>(RecordClass::IN), "Alias.Example.",
                                 now + std::chrono::seconds(30)),
              IoErr::None);

    ASSERT_EQ(cache.lookup_name("EXAMPLE.com.", static_cast<std::uint16_t>(RecordClass::IN), now, snapshot),
              IoErr::None);
    EXPECT_TRUE(snapshot.found());
    ASSERT_TRUE(snapshot.a().present);
    ASSERT_FALSE(snapshot.a().negative);
    ASSERT_EQ(snapshot.a().count, 1);
    EXPECT_TRUE(same_ip(snapshot.a().records[0], a));

    ASSERT_TRUE(snapshot.aaaa().present);
    ASSERT_FALSE(snapshot.aaaa().negative);
    ASSERT_EQ(snapshot.aaaa().count, 1);
    EXPECT_TRUE(same_ip(snapshot.aaaa().records[0], aaaa));

    ASSERT_TRUE(snapshot.cname().present);
    EXPECT_EQ(snapshot.cname().target, "alias.example");
    EXPECT_FALSE(snapshot.has_nxdomain());
}

TEST(DnsCacheTest, NegativeSlotsAreReturned) {
    DnsCache cache;
    ASSERT_TRUE(cache.init());

    NameSnapshot snapshot;
    ASSERT_TRUE(snapshot.init());

    auto now = std::chrono::steady_clock::now();
    ASSERT_EQ(cache.upsert_negative_nodata("nodata.example", static_cast<std::uint16_t>(RecordClass::IN),
                                           static_cast<std::uint16_t>(RecordType::A), now + std::chrono::seconds(20)),
              IoErr::None);
    ASSERT_EQ(cache.upsert_negative_nxdomain("gone.example", static_cast<std::uint16_t>(RecordClass::IN),
                                             now + std::chrono::seconds(40)),
              IoErr::None);

    ASSERT_EQ(cache.lookup_name("nodata.example", static_cast<std::uint16_t>(RecordClass::IN), now, snapshot),
              IoErr::None);
    EXPECT_TRUE(snapshot.found());
    ASSERT_TRUE(snapshot.a().present);
    EXPECT_TRUE(snapshot.a().negative);
    EXPECT_EQ(snapshot.a().count, 0);
    EXPECT_FALSE(snapshot.has_nxdomain());

    ASSERT_EQ(cache.lookup_name("gone.example", static_cast<std::uint16_t>(RecordClass::IN), now, snapshot),
              IoErr::None);
    EXPECT_TRUE(snapshot.found());
    EXPECT_TRUE(snapshot.has_nxdomain());
    EXPECT_FALSE(snapshot.a().present);
    EXPECT_FALSE(snapshot.aaaa().present);
    EXPECT_FALSE(snapshot.cname().present);
}

TEST(DnsCacheTest, ExpiredSlotsAreDroppedDuringLookup) {
    DnsCache cache;
    ASSERT_TRUE(cache.init());

    NameSnapshot snapshot;
    ASSERT_TRUE(snapshot.init());

    auto now = std::chrono::steady_clock::now();
    IpAddress a = IpAddress::v4({10, 0, 0, 1});
    std::array<std::uint8_t, 16> v6_bytes{};
    v6_bytes[0] = 0x20;
    v6_bytes[1] = 0x01;
    v6_bytes[2] = 0x0d;
    v6_bytes[3] = 0xb8;
    v6_bytes[15] = 2;
    IpAddress aaaa = IpAddress::v6(v6_bytes);

    ASSERT_EQ(cache.upsert_a("expire.example", static_cast<std::uint16_t>(RecordClass::IN), &a, 1,
                             now - std::chrono::seconds(1)),
              IoErr::None);
    ASSERT_EQ(cache.upsert_aaaa("expire.example", static_cast<std::uint16_t>(RecordClass::IN), &aaaa, 1,
                                now + std::chrono::seconds(10)),
              IoErr::None);

    ASSERT_EQ(cache.lookup_name("expire.example", static_cast<std::uint16_t>(RecordClass::IN), now, snapshot),
              IoErr::None);
    EXPECT_TRUE(snapshot.found());
    EXPECT_FALSE(snapshot.a().present);
    ASSERT_TRUE(snapshot.aaaa().present);
    EXPECT_TRUE(same_ip(snapshot.aaaa().records[0], aaaa));
}

TEST(DnsCacheTest, PeekNameDoesNotReclaimExpiredEntries) {
    DnsCache cache;
    ASSERT_TRUE(cache.init());

    NameSnapshot snapshot;
    ASSERT_TRUE(snapshot.init());

    auto now = std::chrono::steady_clock::now();
    IpAddress a = IpAddress::v4({10, 0, 0, 2});

    ASSERT_EQ(cache.upsert_a("stale.example", static_cast<std::uint16_t>(RecordClass::IN), &a, 1,
                             now - std::chrono::seconds(1)),
              IoErr::None);
    ASSERT_EQ(cache.entry_count(), 1u);

    ASSERT_EQ(cache.peek_name("stale.example", static_cast<std::uint16_t>(RecordClass::IN), now, snapshot),
              IoErr::None);
    EXPECT_FALSE(snapshot.found());
    EXPECT_EQ(cache.entry_count(), 1u);

    ASSERT_EQ(cache.lookup_name("stale.example", static_cast<std::uint16_t>(RecordClass::IN), now, snapshot),
              IoErr::None);
    EXPECT_FALSE(snapshot.found());
    EXPECT_EQ(cache.entry_count(), 0u);
}

TEST(DnsCacheTest, EvictsApproxLeastRecentlyUsedEntry) {
    DnsCache cache;
    DnsCache::Options options;
    options.max_entries = 2;
    options.max_bytes = 4096;
    options.eviction_sample = 2;
    ASSERT_TRUE(cache.init(options));

    NameSnapshot snapshot;
    ASSERT_TRUE(snapshot.init());

    auto now = std::chrono::steady_clock::now();
    IpAddress first = IpAddress::v4({1, 1, 1, 1});
    IpAddress second = IpAddress::v4({2, 2, 2, 2});
    IpAddress third = IpAddress::v4({3, 3, 3, 3});

    ASSERT_EQ(cache.upsert_a("a.example", static_cast<std::uint16_t>(RecordClass::IN), &first, 1,
                             now + std::chrono::seconds(60)),
              IoErr::None);
    ASSERT_EQ(cache.upsert_a("b.example", static_cast<std::uint16_t>(RecordClass::IN), &second, 1,
                             now + std::chrono::seconds(60)),
              IoErr::None);

    ASSERT_EQ(cache.lookup_name("b.example", static_cast<std::uint16_t>(RecordClass::IN), now, snapshot), IoErr::None);
    ASSERT_TRUE(snapshot.found());

    ASSERT_EQ(cache.upsert_a("c.example", static_cast<std::uint16_t>(RecordClass::IN), &third, 1,
                             now + std::chrono::seconds(60)),
              IoErr::None);

    ASSERT_EQ(cache.lookup_name("a.example", static_cast<std::uint16_t>(RecordClass::IN), now, snapshot), IoErr::None);
    EXPECT_FALSE(snapshot.found());

    ASSERT_EQ(cache.lookup_name("b.example", static_cast<std::uint16_t>(RecordClass::IN), now, snapshot), IoErr::None);
    ASSERT_TRUE(snapshot.found());
    EXPECT_TRUE(same_ip(snapshot.a().records[0], second));

    ASSERT_EQ(cache.lookup_name("c.example", static_cast<std::uint16_t>(RecordClass::IN), now, snapshot), IoErr::None);
    ASSERT_TRUE(snapshot.found());
    EXPECT_TRUE(same_ip(snapshot.a().records[0], third));
}

TEST(DnsCacheTest, LookupFailsWhenSnapshotCapacityIsTooSmall) {
    DnsCache cache;
    ASSERT_TRUE(cache.init());

    NameSnapshot snapshot;
    NameSnapshot::Options options;
    options.max_a_records = 1;
    options.max_aaaa_records = 1;
    options.max_name_storage = 64;
    ASSERT_TRUE(snapshot.init(options));

    auto now = std::chrono::steady_clock::now();
    IpAddress records[2] = {IpAddress::v4({4, 4, 4, 4}), IpAddress::v4({8, 8, 8, 8})};
    ASSERT_EQ(cache.upsert_a("wide.example", static_cast<std::uint16_t>(RecordClass::IN), records, 2,
                             now + std::chrono::seconds(20)),
              IoErr::None);

    EXPECT_EQ(cache.lookup_name("wide.example", static_cast<std::uint16_t>(RecordClass::IN), now, snapshot),
              IoErr::NoMem);
}

TEST(DnsCacheTest, NxDomainReplacesAllCachedSlotsForName) {
    DnsCache cache;
    ASSERT_TRUE(cache.init());

    NameSnapshot snapshot;
    ASSERT_TRUE(snapshot.init());

    auto now = std::chrono::steady_clock::now();
    IpAddress a = IpAddress::v4({1, 2, 3, 4});
    std::array<std::uint8_t, 16> v6_bytes{};
    v6_bytes[0] = 0x20;
    v6_bytes[1] = 0x01;
    v6_bytes[2] = 0x0d;
    v6_bytes[3] = 0xb8;
    v6_bytes[15] = 7;
    IpAddress aaaa = IpAddress::v6(v6_bytes);

    ASSERT_EQ(cache.upsert_a("replace.example", static_cast<std::uint16_t>(RecordClass::IN), &a, 1,
                             now + std::chrono::seconds(60)),
              IoErr::None);
    ASSERT_EQ(cache.upsert_aaaa("replace.example", static_cast<std::uint16_t>(RecordClass::IN), &aaaa, 1,
                                now + std::chrono::seconds(60)),
              IoErr::None);
    ASSERT_EQ(cache.upsert_cname("replace.example", static_cast<std::uint16_t>(RecordClass::IN), "target.example",
                                 now + std::chrono::seconds(60)),
              IoErr::None);
    ASSERT_EQ(cache.upsert_negative_nxdomain("replace.example", static_cast<std::uint16_t>(RecordClass::IN),
                                             now + std::chrono::seconds(30)),
              IoErr::None);

    ASSERT_EQ(cache.lookup_name("replace.example", static_cast<std::uint16_t>(RecordClass::IN), now, snapshot),
              IoErr::None);
    EXPECT_TRUE(snapshot.found());
    EXPECT_TRUE(snapshot.has_nxdomain());
    EXPECT_FALSE(snapshot.a().present);
    EXPECT_FALSE(snapshot.aaaa().present);
    EXPECT_FALSE(snapshot.cname().present);
}

TEST(DnsCacheTest, PositiveUpsertReplacesNxDomainState) {
    DnsCache cache;
    ASSERT_TRUE(cache.init());

    NameSnapshot snapshot;
    ASSERT_TRUE(snapshot.init());

    auto now = std::chrono::steady_clock::now();
    IpAddress a = IpAddress::v4({5, 6, 7, 8});
    ASSERT_EQ(cache.upsert_negative_nxdomain("revived.example", static_cast<std::uint16_t>(RecordClass::IN),
                                             now + std::chrono::seconds(30)),
              IoErr::None);
    ASSERT_EQ(cache.upsert_a("revived.example", static_cast<std::uint16_t>(RecordClass::IN), &a, 1,
                             now + std::chrono::seconds(60)),
              IoErr::None);

    ASSERT_EQ(cache.lookup_name("revived.example", static_cast<std::uint16_t>(RecordClass::IN), now, snapshot),
              IoErr::None);
    EXPECT_TRUE(snapshot.found());
    EXPECT_FALSE(snapshot.has_nxdomain());
    ASSERT_TRUE(snapshot.a().present);
    ASSERT_EQ(snapshot.a().count, 1);
    EXPECT_TRUE(same_ip(snapshot.a().records[0], a));
}

TEST(DnsCacheTest, RepeatedEraseAndReinsertKeepsIndexUsable) {
    DnsCache cache;
    DnsCache::Options options;
    options.max_entries = 4;
    options.max_bytes = 4096;
    options.index_capacity = 4;
    ASSERT_TRUE(cache.init(options));

    auto now = std::chrono::steady_clock::now();
    IpAddress address = IpAddress::v4({10, 0, 0, 1});
    for (std::size_t i = 0; i < 64; ++i) {
        const std::string name = "churn-" + std::to_string(i) + ".example";
        ASSERT_EQ(cache.upsert_a(name, static_cast<std::uint16_t>(RecordClass::IN), &address, 1,
                                 now + std::chrono::seconds(60)),
                  IoErr::None);
        ASSERT_EQ(cache.erase(name, static_cast<std::uint16_t>(RecordClass::IN)), IoErr::None);
    }

    NameSnapshot snapshot;
    ASSERT_TRUE(snapshot.init());
    for (std::size_t i = 0; i < options.max_entries; ++i) {
        const std::string name = "final-" + std::to_string(i) + ".example";
        address = IpAddress::v4({10, 0, 0, static_cast<std::uint8_t>(i + 1U)});
        ASSERT_EQ(cache.upsert_a(name, static_cast<std::uint16_t>(RecordClass::IN), &address, 1,
                                 now + std::chrono::seconds(60)),
                  IoErr::None);
    }
    for (std::size_t i = 0; i < options.max_entries; ++i) {
        const std::string name = "final-" + std::to_string(i) + ".example";
        ASSERT_EQ(cache.lookup_name(name, static_cast<std::uint16_t>(RecordClass::IN), now, snapshot), IoErr::None);
        EXPECT_TRUE(snapshot.found());
    }
}

TEST(DnsCacheTest, SweepBudgetLimitsScannedEntries) {
    DnsCache cache;
    DnsCache::Options options;
    options.max_entries = 4;
    options.max_bytes = 4096;
    ASSERT_TRUE(cache.init(options));

    auto now = std::chrono::steady_clock::now();
    IpAddress address = IpAddress::v4({10, 0, 0, 1});
    for (std::size_t i = 0; i < 3; ++i) {
        const std::string name = "live-" + std::to_string(i) + ".example";
        ASSERT_EQ(cache.upsert_a(name, static_cast<std::uint16_t>(RecordClass::IN), &address, 1,
                                 now + std::chrono::seconds(60)),
                  IoErr::None);
    }
    ASSERT_EQ(cache.upsert_a("expired.example", static_cast<std::uint16_t>(RecordClass::IN), &address, 1,
                             now - std::chrono::seconds(1)),
              IoErr::None);

    EXPECT_EQ(cache.sweep_expired(now, 1), 0u);
    EXPECT_EQ(cache.entry_count(), 4u);
    EXPECT_EQ(cache.sweep_expired(now, 4), 1u);
    EXPECT_EQ(cache.entry_count(), 3u);
}
