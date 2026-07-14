#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <string_view>

#include "dns/DnsCache2.h"

namespace {

using fiber::common::IoErr;
using fiber::dns::DnsCache2;
using fiber::dns::DnsCacheKey;
using fiber::dns::DnsCacheOut;
using fiber::dns::DnsCacheOutKind;
using fiber::dns::DnsNegativeKind;
using fiber::net::IpAddress;

DnsCacheKey key(std::string_view name) { return {name, fiber::dns::dns_cache_hash(name)}; }

} // namespace

TEST(DnsCache2Test, ReturnsBothFamiliesInRotatedOrder) {
    DnsCache2 cache;
    ASSERT_TRUE(cache.init());

    const auto now = std::chrono::steady_clock::now();
    const std::array v4 = {IpAddress::v4({1, 1, 1, 1}), IpAddress::v4({2, 2, 2, 2}), IpAddress::v4({3, 3, 3, 3})};
    std::array<std::uint8_t, 16> v6_first_bytes{};
    v6_first_bytes[15] = 1;
    std::array<std::uint8_t, 16> v6_second_bytes{};
    v6_second_bytes[15] = 2;
    const std::array v6 = {IpAddress::v6(v6_first_bytes), IpAddress::v6(v6_second_bytes)};

    ASSERT_EQ(cache.upsert_address_set(key("dual.example"), v4.data(), v4.size(), now + std::chrono::seconds(60)),
              IoErr::None);
    ASSERT_EQ(cache.upsert_address_set(key("dual.example"), v6.data(), v6.size(), now + std::chrono::seconds(120)),
              IoErr::None);

    DnsCacheOut out{};
    ASSERT_EQ(cache.lookup(key("dual.example"), now, out), IoErr::None);
    ASSERT_EQ(out.kind, DnsCacheOutKind::Addresses);
    ASSERT_EQ(out.value.addresses.count, 5);
    ASSERT_EQ(out.value.addresses.v4_count, 3);
    EXPECT_EQ(out.value.addresses.records[0], v4[0]);
    EXPECT_EQ(out.value.addresses.records[1], v4[1]);
    EXPECT_EQ(out.value.addresses.records[2], v4[2]);
    EXPECT_EQ(out.value.addresses.records[3], v6[0]);
    EXPECT_EQ(out.value.addresses.records[4], v6[1]);

    ASSERT_EQ(cache.lookup(key("dual.example"), now, out), IoErr::None);
    ASSERT_EQ(out.kind, DnsCacheOutKind::Addresses);
    EXPECT_EQ(out.value.addresses.records[0], v4[1]);
    EXPECT_EQ(out.value.addresses.records[1], v4[2]);
    EXPECT_EQ(out.value.addresses.records[2], v4[0]);
    EXPECT_EQ(out.value.addresses.records[3], v6[1]);
    EXPECT_EQ(out.value.addresses.records[4], v6[0]);
}

TEST(DnsCache2Test, ExpiresAddressFamiliesIndependently) {
    DnsCache2 cache;
    ASSERT_TRUE(cache.init());

    const auto now = std::chrono::steady_clock::now();
    const IpAddress v4 = IpAddress::v4({4, 4, 4, 4});
    std::array<std::uint8_t, 16> v6_bytes{};
    v6_bytes[15] = 6;
    const IpAddress v6 = IpAddress::v6(v6_bytes);
    ASSERT_EQ(cache.upsert_address_set(key("expiry.example"), &v4, 1, now + std::chrono::seconds(10)), IoErr::None);
    ASSERT_EQ(cache.upsert_address_set(key("expiry.example"), &v6, 1, now + std::chrono::seconds(20)), IoErr::None);

    DnsCacheOut out{};
    ASSERT_EQ(cache.lookup(key("expiry.example"), now + std::chrono::seconds(11), out), IoErr::None);
    ASSERT_EQ(out.kind, DnsCacheOutKind::Addresses);
    ASSERT_EQ(out.value.addresses.count, 1);
    EXPECT_EQ(out.value.addresses.v4_count, 0);
    EXPECT_EQ(out.value.addresses.records[0], v6);

    ASSERT_EQ(cache.lookup(key("expiry.example"), now + std::chrono::seconds(21), out), IoErr::None);
    EXPECT_EQ(out.kind, DnsCacheOutKind::Miss);
    EXPECT_EQ(cache.entry_count(), 0u);
}

TEST(DnsCache2Test, MaintainsMutuallyExclusiveAddressCnameAndNegativeStates) {
    DnsCache2 cache;
    ASSERT_TRUE(cache.init());

    const auto now = std::chrono::steady_clock::now();
    const IpAddress address = IpAddress::v4({7, 7, 7, 7});
    ASSERT_EQ(cache.upsert_address_set(key("state.example"), &address, 1, now + std::chrono::seconds(60)), IoErr::None);
    ASSERT_EQ(cache.upsert_cname(key("state.example"), "target.example", now + std::chrono::seconds(50)), IoErr::None);

    DnsCacheOut out{};
    ASSERT_EQ(cache.lookup(key("state.example"), now, out), IoErr::None);
    ASSERT_EQ(out.kind, DnsCacheOutKind::Cname);
    EXPECT_EQ(std::string_view(out.value.cname.buf, out.value.cname.length), "target.example");
    EXPECT_EQ(out.value.cname.buf[out.value.cname.length], '\0');

    ASSERT_EQ(cache.upsert_negative(key("state.example"), DnsNegativeKind::NxDomain, now + std::chrono::seconds(30)),
              IoErr::None);
    ASSERT_EQ(cache.lookup(key("state.example"), now, out), IoErr::None);
    ASSERT_EQ(out.kind, DnsCacheOutKind::Negative);
    EXPECT_EQ(out.value.negative, DnsNegativeKind::NxDomain);

    ASSERT_EQ(cache.upsert_address_set(key("state.example"), &address, 1, now + std::chrono::seconds(70)), IoErr::None);
    ASSERT_EQ(cache.lookup(key("state.example"), now, out), IoErr::None);
    ASSERT_EQ(out.kind, DnsCacheOutKind::Addresses);
    ASSERT_EQ(out.value.addresses.count, 1);
    EXPECT_EQ(out.value.addresses.records[0], address);
}

TEST(DnsCache2Test, ExpiresNegativeEntries) {
    DnsCache2 cache;
    ASSERT_TRUE(cache.init());

    const auto now = std::chrono::steady_clock::now();
    ASSERT_EQ(cache.upsert_negative(key("missing.example"), DnsNegativeKind::NoAddress, now + std::chrono::seconds(10)),
              IoErr::None);

    DnsCacheOut out{};
    ASSERT_EQ(cache.lookup(key("missing.example"), now, out), IoErr::None);
    ASSERT_EQ(out.kind, DnsCacheOutKind::Negative);
    EXPECT_EQ(out.value.negative, DnsNegativeKind::NoAddress);

    ASSERT_EQ(cache.lookup(key("missing.example"), now + std::chrono::seconds(10), out), IoErr::None);
    EXPECT_EQ(out.kind, DnsCacheOutKind::Miss);
    EXPECT_EQ(cache.entry_count(), 0u);
}

TEST(DnsCache2Test, UsesExactLruEviction) {
    DnsCache2 cache;
    DnsCache2::Options options;
    options.max_entries = 2;
    options.max_bytes = 4096;
    options.bucket_count = 2;
    ASSERT_TRUE(cache.init(options));

    const auto now = std::chrono::steady_clock::now();
    const IpAddress first = IpAddress::v4({1, 0, 0, 1});
    const IpAddress second = IpAddress::v4({2, 0, 0, 2});
    const IpAddress third = IpAddress::v4({3, 0, 0, 3});
    ASSERT_EQ(cache.upsert_address_set(key("first.example"), &first, 1, now + std::chrono::seconds(60)), IoErr::None);
    ASSERT_EQ(cache.upsert_address_set(key("second.example"), &second, 1, now + std::chrono::seconds(60)), IoErr::None);

    DnsCacheOut out{};
    ASSERT_EQ(cache.lookup(key("first.example"), now, out), IoErr::None);
    ASSERT_EQ(out.kind, DnsCacheOutKind::Addresses);
    ASSERT_EQ(cache.upsert_address_set(key("third.example"), &third, 1, now + std::chrono::seconds(60)), IoErr::None);

    ASSERT_EQ(cache.lookup(key("second.example"), now, out), IoErr::None);
    EXPECT_EQ(out.kind, DnsCacheOutKind::Miss);
    ASSERT_EQ(cache.lookup(key("first.example"), now, out), IoErr::None);
    EXPECT_EQ(out.kind, DnsCacheOutKind::Addresses);
    ASSERT_EQ(cache.lookup(key("third.example"), now, out), IoErr::None);
    EXPECT_EQ(out.kind, DnsCacheOutKind::Addresses);
}

TEST(DnsCache2Test, EvictsLeastRecentlyUsedEntryToHonorByteLimit) {
    const auto now = std::chrono::steady_clock::now();
    const IpAddress address = IpAddress::v4({192, 0, 2, 1});

    DnsCache2 probe;
    DnsCache2::Options probe_options;
    probe_options.max_entries = 3;
    probe_options.max_bytes = 4096;
    probe_options.bucket_count = 1;
    ASSERT_TRUE(probe.init(probe_options));
    ASSERT_EQ(probe.upsert_address_set(key("one.example"), &address, 1, now + std::chrono::seconds(60)), IoErr::None);
    ASSERT_EQ(probe.upsert_address_set(key("two.example"), &address, 1, now + std::chrono::seconds(60)), IoErr::None);
    const std::size_t two_entry_bytes = probe.bytes_used();

    DnsCache2 cache;
    DnsCache2::Options options;
    options.max_entries = 3;
    options.max_bytes = two_entry_bytes;
    options.bucket_count = 1;
    ASSERT_TRUE(cache.init(options));
    ASSERT_EQ(cache.upsert_address_set(key("one.example"), &address, 1, now + std::chrono::seconds(60)), IoErr::None);
    ASSERT_EQ(cache.upsert_address_set(key("two.example"), &address, 1, now + std::chrono::seconds(60)), IoErr::None);
    ASSERT_EQ(cache.bytes_used(), two_entry_bytes);
    ASSERT_EQ(cache.upsert_address_set(key("six.example"), &address, 1, now + std::chrono::seconds(60)), IoErr::None);
    EXPECT_LE(cache.bytes_used(), options.max_bytes);

    DnsCacheOut out{};
    ASSERT_EQ(cache.lookup(key("one.example"), now, out), IoErr::None);
    EXPECT_EQ(out.kind, DnsCacheOutKind::Miss);
    ASSERT_EQ(cache.lookup(key("two.example"), now, out), IoErr::None);
    EXPECT_EQ(out.kind, DnsCacheOutKind::Addresses);
    ASSERT_EQ(cache.lookup(key("six.example"), now, out), IoErr::None);
    EXPECT_EQ(out.kind, DnsCacheOutKind::Addresses);
}

TEST(DnsCache2Test, RejectsOperationsBeforeInitialization) {
    DnsCache2 cache;
    const auto expire_at = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    const IpAddress address = IpAddress::loopback_v4();
    DnsCacheOut out{};

    EXPECT_EQ(cache.lookup(key("uninitialized.example"), {}, out), IoErr::Invalid);
    EXPECT_EQ(cache.upsert_address_set(key("uninitialized.example"), &address, 1, expire_at), IoErr::Invalid);
    EXPECT_EQ(cache.upsert_cname(key("uninitialized.example"), "target.example", expire_at), IoErr::Invalid);
    EXPECT_EQ(cache.upsert_negative(key("uninitialized.example"), DnsNegativeKind::NxDomain, expire_at),
              IoErr::Invalid);
    EXPECT_EQ(cache.erase(key("uninitialized.example")), IoErr::Invalid);
}

TEST(DnsCache2Test, ComparesNamesWhenHashesCollide) {
    DnsCache2 cache;
    ASSERT_TRUE(cache.init());

    const auto now = std::chrono::steady_clock::now();
    const DnsCacheKey first_key{"first-collision.example", 42};
    const DnsCacheKey second_key{"second-collision.example", 42};
    const IpAddress first = IpAddress::v4({10, 0, 0, 1});
    const IpAddress second = IpAddress::v4({10, 0, 0, 2});
    ASSERT_EQ(cache.upsert_address_set(first_key, &first, 1, now + std::chrono::seconds(60)), IoErr::None);
    ASSERT_EQ(cache.upsert_address_set(second_key, &second, 1, now + std::chrono::seconds(60)), IoErr::None);

    DnsCacheOut out{};
    ASSERT_EQ(cache.lookup(first_key, now, out), IoErr::None);
    ASSERT_EQ(out.kind, DnsCacheOutKind::Addresses);
    EXPECT_EQ(out.value.addresses.records[0], first);
    ASSERT_EQ(cache.lookup(second_key, now, out), IoErr::None);
    ASSERT_EQ(out.kind, DnsCacheOutKind::Addresses);
    EXPECT_EQ(out.value.addresses.records[0], second);
}

TEST(DnsCache2Test, RejectsMixedOrOversizedAddressSets) {
    DnsCache2 cache;
    ASSERT_TRUE(cache.init());

    const auto expire_at = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    const std::array mixed = {IpAddress::v4({1, 1, 1, 1}), IpAddress::loopback_v6()};
    EXPECT_EQ(cache.upsert_address_set(key("mixed.example"), mixed.data(), mixed.size(), expire_at), IoErr::Invalid);

    std::array<IpAddress, fiber::dns::kDnsCacheMaxAddressesPerFamily + 1> oversized{};
    for (std::size_t i = 0; i < oversized.size(); ++i) {
        oversized[i] = IpAddress::v4({192, 0, 2, static_cast<std::uint8_t>(i + 1U)});
    }
    EXPECT_EQ(cache.upsert_address_set(key("oversized.example"), oversized.data(), oversized.size(), expire_at),
              IoErr::MessageTooLarge);

    EXPECT_EQ(cache.upsert_negative(key("invalid-negative.example"), static_cast<DnsNegativeKind>(255), expire_at),
              IoErr::Invalid);
}
