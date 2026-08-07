#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <limits>
#include <string_view>

#include <fiber/dns/DnsCache2.h>

namespace {

using fiber::common::IoErr;
using fiber::dns::DnsCache2;
using fiber::dns::DnsCacheKey;
using fiber::dns::DnsCacheOut;
using fiber::dns::DnsCacheOutKind;
using fiber::net::IpAddress;
using fiber::net::IpFamily;

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

    ASSERT_EQ(cache.upsert_address_set(key("dual.example"), IpFamily::V4, v4.data(), v4.size(),
                                       now + std::chrono::seconds(60)),
              IoErr::None);
    ASSERT_EQ(cache.upsert_address_set(key("dual.example"), IpFamily::V6, v6.data(), v6.size(),
                                       now + std::chrono::seconds(120)),
              IoErr::None);

    DnsCacheOut out{};
    ASSERT_EQ(cache.lookup(key("dual.example"), now, out), IoErr::None);
    ASSERT_EQ(out.kind, DnsCacheOutKind::Addresses);
    ASSERT_TRUE(out.value.addresses.has_v4());
    ASSERT_TRUE(out.value.addresses.has_v6());
    EXPECT_EQ(out.value.addresses.v4_expire_at, now + std::chrono::seconds(60));
    EXPECT_EQ(out.value.addresses.v6_expire_at, now + std::chrono::seconds(120));
    ASSERT_EQ(out.value.addresses.address_set.count, 5);
    ASSERT_EQ(out.value.addresses.address_set.v4_count, 3);
    EXPECT_EQ(out.value.addresses.address_set.records[0], v4[0]);
    EXPECT_EQ(out.value.addresses.address_set.records[1], v4[1]);
    EXPECT_EQ(out.value.addresses.address_set.records[2], v4[2]);
    EXPECT_EQ(out.value.addresses.address_set.records[3], v6[0]);
    EXPECT_EQ(out.value.addresses.address_set.records[4], v6[1]);

    ASSERT_EQ(cache.lookup(key("dual.example"), now, out), IoErr::None);
    ASSERT_EQ(out.kind, DnsCacheOutKind::Addresses);
    EXPECT_EQ(out.value.addresses.address_set.records[0], v4[1]);
    EXPECT_EQ(out.value.addresses.address_set.records[1], v4[2]);
    EXPECT_EQ(out.value.addresses.address_set.records[2], v4[0]);
    EXPECT_EQ(out.value.addresses.address_set.records[3], v6[1]);
    EXPECT_EQ(out.value.addresses.address_set.records[4], v6[0]);
}

TEST(DnsCache2Test, ExpiresAddressFamiliesIndependently) {
    DnsCache2 cache;
    ASSERT_TRUE(cache.init());

    const auto now = std::chrono::steady_clock::now();
    const IpAddress v4 = IpAddress::v4({4, 4, 4, 4});
    std::array<std::uint8_t, 16> v6_bytes{};
    v6_bytes[15] = 6;
    const IpAddress v6 = IpAddress::v6(v6_bytes);
    ASSERT_EQ(cache.upsert_address_set(key("expiry.example"), IpFamily::V4, &v4, 1, now + std::chrono::seconds(10)),
              IoErr::None);
    ASSERT_EQ(cache.upsert_address_set(key("expiry.example"), IpFamily::V6, &v6, 1, now + std::chrono::seconds(20)),
              IoErr::None);

    DnsCacheOut out{};
    ASSERT_EQ(cache.lookup(key("expiry.example"), now + std::chrono::seconds(11), out), IoErr::None);
    ASSERT_EQ(out.kind, DnsCacheOutKind::Addresses);
    EXPECT_FALSE(out.value.addresses.has_v4());
    EXPECT_TRUE(out.value.addresses.has_v6());
    ASSERT_EQ(out.value.addresses.address_set.count, 1);
    EXPECT_EQ(out.value.addresses.address_set.v4_count, 0);
    EXPECT_EQ(out.value.addresses.address_set.records[0], v6);

    ASSERT_EQ(cache.lookup(key("expiry.example"), now + std::chrono::seconds(21), out), IoErr::None);
    EXPECT_EQ(out.kind, DnsCacheOutKind::Miss);
    EXPECT_EQ(cache.entry_count(), 0u);
}

TEST(DnsCache2Test, DistinguishesNoDataFromMissingFamily) {
    DnsCache2 cache;
    ASSERT_TRUE(cache.init());

    const auto now = std::chrono::steady_clock::now();
    ASSERT_EQ(cache.upsert_address_set(key("nodata.example"), IpFamily::V4, nullptr, 0, now + std::chrono::seconds(10)),
              IoErr::None);

    DnsCacheOut out{};
    ASSERT_EQ(cache.lookup(key("nodata.example"), now, out), IoErr::None);
    ASSERT_EQ(out.kind, DnsCacheOutKind::Addresses);
    EXPECT_TRUE(out.value.addresses.has_v4());
    EXPECT_FALSE(out.value.addresses.has_v6());
    EXPECT_EQ(out.value.addresses.address_set.count, 0);
    EXPECT_EQ(out.value.addresses.v4_expire_at, now + std::chrono::seconds(10));

    const IpAddress v6 = IpAddress::loopback_v6();
    ASSERT_EQ(cache.upsert_address_set(key("nodata.example"), IpFamily::V6, &v6, 1, now + std::chrono::seconds(20)),
              IoErr::None);
    ASSERT_EQ(cache.lookup(key("nodata.example"), now, out), IoErr::None);
    ASSERT_EQ(out.kind, DnsCacheOutKind::Addresses);
    EXPECT_TRUE(out.value.addresses.has_v4());
    EXPECT_TRUE(out.value.addresses.has_v6());
    ASSERT_EQ(out.value.addresses.address_set.count, 1);
    EXPECT_EQ(out.value.addresses.address_set.v4_count, 0);
    EXPECT_EQ(out.value.addresses.address_set.records[0], v6);

    ASSERT_EQ(cache.lookup(key("nodata.example"), now + std::chrono::seconds(11), out), IoErr::None);
    ASSERT_EQ(out.kind, DnsCacheOutKind::Addresses);
    EXPECT_FALSE(out.value.addresses.has_v4());
    EXPECT_TRUE(out.value.addresses.has_v6());
}

TEST(DnsCache2Test, ReplacesPositiveAndNoDataPerFamily) {
    DnsCache2 cache;
    ASSERT_TRUE(cache.init());

    const auto now = std::chrono::steady_clock::now();
    const IpAddress v4 = IpAddress::v4({4, 3, 2, 1});
    ASSERT_EQ(cache.upsert_address_set(key("replace-family.example"), IpFamily::V4, &v4, 1,
                                       now + std::chrono::seconds(10)),
              IoErr::None);
    ASSERT_EQ(cache.upsert_address_set(key("replace-family.example"), IpFamily::V6, nullptr, 0,
                                       now + std::chrono::seconds(20)),
              IoErr::None);

    DnsCacheOut out{};
    ASSERT_EQ(cache.lookup(key("replace-family.example"), now, out), IoErr::None);
    ASSERT_EQ(out.kind, DnsCacheOutKind::Addresses);
    EXPECT_TRUE(out.value.addresses.has_v4());
    EXPECT_TRUE(out.value.addresses.has_v6());
    ASSERT_EQ(out.value.addresses.address_set.count, 1);
    EXPECT_EQ(out.value.addresses.address_set.v4_count, 1);

    ASSERT_EQ(cache.upsert_address_set(key("replace-family.example"), IpFamily::V4, nullptr, 0,
                                       now + std::chrono::seconds(30)),
              IoErr::None);
    ASSERT_EQ(cache.lookup(key("replace-family.example"), now, out), IoErr::None);
    ASSERT_EQ(out.kind, DnsCacheOutKind::Addresses);
    EXPECT_TRUE(out.value.addresses.has_v4());
    EXPECT_TRUE(out.value.addresses.has_v6());
    EXPECT_EQ(out.value.addresses.address_set.count, 0);

    ASSERT_EQ(cache.lookup(key("replace-family.example"), now + std::chrono::seconds(21), out), IoErr::None);
    ASSERT_EQ(out.kind, DnsCacheOutKind::Addresses);
    EXPECT_TRUE(out.value.addresses.has_v4());
    EXPECT_FALSE(out.value.addresses.has_v6());
    EXPECT_EQ(out.value.addresses.address_set.count, 0);

    ASSERT_EQ(cache.upsert_address_set(key("replace-family.example"), IpFamily::V4, &v4, 1,
                                       now + std::chrono::seconds(40)),
              IoErr::None);
    ASSERT_EQ(cache.lookup(key("replace-family.example"), now, out), IoErr::None);
    EXPECT_TRUE(out.value.addresses.has_v4());
    EXPECT_FALSE(out.value.addresses.has_v6());
    ASSERT_EQ(out.value.addresses.address_set.count, 1);
    EXPECT_EQ(out.value.addresses.address_set.records[0], v4);
    EXPECT_EQ(out.value.addresses.v4_expire_at, now + std::chrono::seconds(40));
}

TEST(DnsCache2Test, MaintainsMutuallyExclusiveAddressCnameAndNxDomainStates) {
    DnsCache2 cache;
    ASSERT_TRUE(cache.init());

    const auto now = std::chrono::steady_clock::now();
    const IpAddress address = IpAddress::v4({7, 7, 7, 7});
    ASSERT_EQ(cache.upsert_address_set(key("state.example"), IpFamily::V4, &address, 1, now + std::chrono::seconds(60)),
              IoErr::None);
    ASSERT_EQ(cache.upsert_cname(key("state.example"), "target.example", now + std::chrono::seconds(50)), IoErr::None);

    DnsCacheOut out{};
    ASSERT_EQ(cache.lookup(key("state.example"), now, out), IoErr::None);
    ASSERT_EQ(out.kind, DnsCacheOutKind::Cname);
    EXPECT_EQ(std::string_view(out.value.cname.buf, out.value.cname.length), "target.example");
    EXPECT_EQ(out.value.cname.buf[out.value.cname.length], '\0');

    ASSERT_EQ(cache.upsert_nxdomain(key("state.example"), now + std::chrono::seconds(30)), IoErr::None);
    ASSERT_EQ(cache.lookup(key("state.example"), now, out), IoErr::None);
    ASSERT_EQ(out.kind, DnsCacheOutKind::NxDomain);

    ASSERT_EQ(cache.upsert_address_set(key("state.example"), IpFamily::V4, &address, 1, now + std::chrono::seconds(70)),
              IoErr::None);
    ASSERT_EQ(cache.lookup(key("state.example"), now, out), IoErr::None);
    ASSERT_EQ(out.kind, DnsCacheOutKind::Addresses);
    ASSERT_EQ(out.value.addresses.address_set.count, 1);
    EXPECT_EQ(out.value.addresses.address_set.records[0], address);
}

TEST(DnsCache2Test, ExpiresNxDomainEntries) {
    DnsCache2 cache;
    ASSERT_TRUE(cache.init());

    const auto now = std::chrono::steady_clock::now();
    ASSERT_EQ(cache.upsert_nxdomain(key("missing.example"), now + std::chrono::seconds(10)), IoErr::None);

    DnsCacheOut out{};
    ASSERT_EQ(cache.lookup(key("missing.example"), now, out), IoErr::None);
    ASSERT_EQ(out.kind, DnsCacheOutKind::NxDomain);

    ASSERT_EQ(cache.lookup(key("missing.example"), now + std::chrono::seconds(10), out), IoErr::None);
    EXPECT_EQ(out.kind, DnsCacheOutKind::Miss);
    EXPECT_EQ(cache.entry_count(), 0u);
}

TEST(DnsCache2Test, EvictsEntryWithNearestExpirationAtEntryLimit) {
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
    ASSERT_EQ(cache.upsert_address_set(key("first.example"), IpFamily::V4, &first, 1, now + std::chrono::seconds(120)),
              IoErr::None);
    ASSERT_EQ(cache.upsert_address_set(key("second.example"), IpFamily::V4, &second, 1, now + std::chrono::seconds(30)),
              IoErr::None);

    DnsCacheOut out{};
    ASSERT_EQ(cache.lookup(key("first.example"), now, out), IoErr::None);
    ASSERT_EQ(out.kind, DnsCacheOutKind::Addresses);
    ASSERT_EQ(cache.upsert_address_set(key("third.example"), IpFamily::V4, &third, 1, now + std::chrono::seconds(60)),
              IoErr::None);

    ASSERT_EQ(cache.lookup(key("second.example"), now, out), IoErr::None);
    EXPECT_EQ(out.kind, DnsCacheOutKind::Miss);
    ASSERT_EQ(cache.lookup(key("first.example"), now, out), IoErr::None);
    EXPECT_EQ(out.kind, DnsCacheOutKind::Addresses);
    ASSERT_EQ(cache.lookup(key("third.example"), now, out), IoErr::None);
    EXPECT_EQ(out.kind, DnsCacheOutKind::Addresses);
}

TEST(DnsCache2Test, UpdatingExpirationChangesEvictionPriority) {
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
    ASSERT_EQ(cache.upsert_address_set(key("first.example"), IpFamily::V4, &first, 1, now + std::chrono::seconds(10)),
              IoErr::None);
    ASSERT_EQ(cache.upsert_address_set(key("second.example"), IpFamily::V4, &second, 1, now + std::chrono::seconds(20)),
              IoErr::None);
    ASSERT_EQ(cache.upsert_address_set(key("first.example"), IpFamily::V4, &first, 1, now + std::chrono::seconds(30)),
              IoErr::None);
    ASSERT_EQ(cache.upsert_address_set(key("third.example"), IpFamily::V4, &third, 1, now + std::chrono::seconds(40)),
              IoErr::None);

    DnsCacheOut out{};
    ASSERT_EQ(cache.lookup(key("first.example"), now, out), IoErr::None);
    EXPECT_EQ(out.kind, DnsCacheOutKind::Addresses);
    ASSERT_EQ(cache.lookup(key("second.example"), now, out), IoErr::None);
    EXPECT_EQ(out.kind, DnsCacheOutKind::Miss);
    ASSERT_EQ(cache.lookup(key("third.example"), now, out), IoErr::None);
    EXPECT_EQ(out.kind, DnsCacheOutKind::Addresses);
}

TEST(DnsCache2Test, EvictsEntryWithNearestExpirationToHonorByteLimit) {
    const auto now = std::chrono::steady_clock::now();
    const IpAddress address = IpAddress::v4({192, 0, 2, 1});

    DnsCache2 probe;
    DnsCache2::Options probe_options;
    probe_options.max_entries = 3;
    probe_options.max_bytes = 4096;
    probe_options.bucket_count = 1;
    ASSERT_TRUE(probe.init(probe_options));
    ASSERT_EQ(probe.upsert_address_set(key("one.example"), IpFamily::V4, &address, 1, now + std::chrono::seconds(10)),
              IoErr::None);
    ASSERT_EQ(probe.upsert_address_set(key("two.example"), IpFamily::V4, &address, 1, now + std::chrono::seconds(20)),
              IoErr::None);
    const std::size_t two_entry_bytes = probe.bytes_used();

    DnsCache2 cache;
    DnsCache2::Options options;
    options.max_entries = 3;
    options.max_bytes = two_entry_bytes;
    options.bucket_count = 1;
    ASSERT_TRUE(cache.init(options));
    ASSERT_EQ(cache.upsert_address_set(key("one.example"), IpFamily::V4, &address, 1, now + std::chrono::seconds(10)),
              IoErr::None);
    ASSERT_EQ(cache.upsert_address_set(key("two.example"), IpFamily::V4, &address, 1, now + std::chrono::seconds(20)),
              IoErr::None);
    ASSERT_EQ(cache.bytes_used(), two_entry_bytes);
    ASSERT_EQ(cache.upsert_address_set(key("six.example"), IpFamily::V4, &address, 1, now + std::chrono::seconds(30)),
              IoErr::None);
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
    EXPECT_EQ(cache.upsert_address_set(key("uninitialized.example"), IpFamily::V4, &address, 1, expire_at),
              IoErr::Invalid);
    EXPECT_EQ(cache.upsert_cname(key("uninitialized.example"), "target.example", expire_at), IoErr::Invalid);
    EXPECT_EQ(cache.upsert_nxdomain(key("uninitialized.example"), expire_at), IoErr::Invalid);
    EXPECT_EQ(cache.erase(key("uninitialized.example")), IoErr::Invalid);
}

TEST(DnsCache2Test, SizesDefaultBucketsForHalfMaximumLoad) {
    DnsCache2 cache;
    DnsCache2::Options options;
    options.max_entries = 3;
    options.max_bytes = 4096;
    ASSERT_TRUE(cache.init(options));

    EXPECT_EQ(cache.bucket_count(), 8u);
}

TEST(DnsCache2Test, RejectsOverflowInDefaultBucketSizing) {
    DnsCache2 cache;
    DnsCache2::Options options;
    options.max_entries = std::numeric_limits<std::size_t>::max();
    options.max_bytes = std::numeric_limits<std::size_t>::max();

    EXPECT_FALSE(cache.init(options));
    EXPECT_EQ(cache.bucket_count(), 0u);
    EXPECT_EQ(cache.bytes_used(), 0u);
}

TEST(DnsCache2Test, ComparesNamesWhenHashesCollide) {
    DnsCache2 cache;
    ASSERT_TRUE(cache.init());

    const auto now = std::chrono::steady_clock::now();
    const DnsCacheKey first_key{"first-collision.example", 42};
    const DnsCacheKey second_key{"second-collision.example", 42};
    const IpAddress first = IpAddress::v4({10, 0, 0, 1});
    const IpAddress second = IpAddress::v4({10, 0, 0, 2});
    ASSERT_EQ(cache.upsert_address_set(first_key, IpFamily::V4, &first, 1, now + std::chrono::seconds(60)),
              IoErr::None);
    ASSERT_EQ(cache.upsert_address_set(second_key, IpFamily::V4, &second, 1, now + std::chrono::seconds(60)),
              IoErr::None);

    DnsCacheOut out{};
    ASSERT_EQ(cache.lookup(first_key, now, out), IoErr::None);
    ASSERT_EQ(out.kind, DnsCacheOutKind::Addresses);
    EXPECT_EQ(out.value.addresses.address_set.records[0], first);
    ASSERT_EQ(cache.lookup(second_key, now, out), IoErr::None);
    ASSERT_EQ(out.kind, DnsCacheOutKind::Addresses);
    EXPECT_EQ(out.value.addresses.address_set.records[0], second);
}

TEST(DnsCache2Test, RejectsMixedOrOversizedAddressSets) {
    DnsCache2 cache;
    ASSERT_TRUE(cache.init());

    const auto expire_at = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    const std::array mixed = {IpAddress::v4({1, 1, 1, 1}), IpAddress::loopback_v6()};
    EXPECT_EQ(cache.upsert_address_set(key("mixed.example"), IpFamily::V4, mixed.data(), mixed.size(), expire_at),
              IoErr::Invalid);

    std::array<IpAddress, fiber::dns::kDnsMaxAddressesPerFamily + 1> oversized{};
    for (std::size_t i = 0; i < oversized.size(); ++i) {
        oversized[i] = IpAddress::v4({192, 0, 2, static_cast<std::uint8_t>(i + 1U)});
    }
    EXPECT_EQ(cache.upsert_address_set(key("oversized.example"), IpFamily::V4, oversized.data(), oversized.size(),
                                       expire_at),
              IoErr::MessageTooLarge);
    EXPECT_EQ(
            cache.upsert_address_set(key("invalid-family.example"), static_cast<IpFamily>(255), nullptr, 0, expire_at),
            IoErr::Invalid);
}

TEST(DnsCache2Test, ReinitializesWithEmptyInlineExpiryHeap) {
    DnsCache2 cache;
    ASSERT_TRUE(cache.init());

    const auto now = std::chrono::steady_clock::now();
    const IpAddress address = IpAddress::loopback_v4();
    ASSERT_EQ(cache.upsert_address_set(key("before-release.example"), IpFamily::V4, &address, 1,
                                       now + std::chrono::seconds(60)),
              IoErr::None);
    ASSERT_EQ(cache.upsert_nxdomain(key("nxdomain-before-release.example"), now + std::chrono::seconds(30)),
              IoErr::None);

    cache.release();
    EXPECT_EQ(cache.entry_count(), 0u);
    EXPECT_EQ(cache.bytes_used(), 0u);
    EXPECT_EQ(cache.bucket_count(), 0u);

    ASSERT_TRUE(cache.init());
    DnsCacheOut out{};
    ASSERT_EQ(cache.lookup(key("before-release.example"), now, out), IoErr::None);
    EXPECT_EQ(out.kind, DnsCacheOutKind::Miss);
    ASSERT_EQ(cache.upsert_address_set(key("after-release.example"), IpFamily::V4, &address, 1,
                                       now + std::chrono::seconds(60)),
              IoErr::None);
    EXPECT_EQ(cache.entry_count(), 1u);
}
