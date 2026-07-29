#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "http/Http1ConnectionBucketIndex.h"

namespace {

using fiber::common::IoErr;
using fiber::http::Http1ConnectionBucketIndex;
using fiber::http::Http1ConnectionGroupKey;
using fiber::http::Http1ConnectionPoolGroupBucket;

Http1ConnectionGroupKey make_ip_key(std::uint8_t last_octet, std::uint16_t port = 80,
                                    Http1ConnectionGroupKey::Scheme scheme = Http1ConnectionGroupKey::Scheme::Http) {
    return Http1ConnectionGroupKey::from_ip(fiber::net::IpAddress::v4({127, 0, 0, last_octet}), port, scheme);
}

std::size_t home_bucket(const Http1ConnectionGroupKey &key, std::size_t slot_capacity) {
    return static_cast<std::size_t>(key.hash()) & (slot_capacity - 1U);
}

std::array<std::uint8_t, 3> find_colliding_ip_suffixes(std::size_t slot_capacity) {
    std::array<std::uint8_t, 3> out{};
    const std::size_t target_bucket = home_bucket(make_ip_key(1), slot_capacity);
    std::size_t found = 0;

    for (std::uint16_t octet = 1; octet <= 255 && found < out.size(); ++octet) {
        const auto key = make_ip_key(static_cast<std::uint8_t>(octet));
        if (home_bucket(key, slot_capacity) == target_bucket) {
            out[found++] = static_cast<std::uint8_t>(octet);
        }
    }

    EXPECT_EQ(found, out.size());
    return out;
}

} // namespace

TEST(Http1ConnectionBucketIndexTest, InitializesRequestedCapacityAsPowerOfTwo) {
    Http1ConnectionBucketIndex index;

    ASSERT_TRUE(index.init(3));
    EXPECT_EQ(index.slot_capacity(), 8u);
    EXPECT_TRUE(index.empty());
}

TEST(Http1ConnectionBucketIndexTest, InsertsFindsAndRejectsDuplicateKeys) {
    Http1ConnectionPoolGroupBucket bucket1;
    Http1ConnectionPoolGroupBucket bucket2;
    Http1ConnectionPoolGroupBucket duplicate_bucket;
    Http1ConnectionBucketIndex index;
    ASSERT_TRUE(index.init(2));

    const auto key1 = make_ip_key(1, 80, Http1ConnectionGroupKey::Scheme::Http);
    const auto key2 = make_ip_key(2, 443, Http1ConnectionGroupKey::Scheme::Https);

    EXPECT_EQ(index.insert(key1, bucket1), IoErr::None);
    EXPECT_EQ(index.insert(key2, bucket2), IoErr::None);
    EXPECT_EQ(index.insert(key1, duplicate_bucket), IoErr::Already);

    auto entry1 = index.find(key1);
    auto entry2 = index.find(key2);
    EXPECT_TRUE(entry1);
    EXPECT_TRUE(entry2);
    EXPECT_EQ(entry1.bucket, &bucket1);
    EXPECT_EQ(entry2.bucket, &bucket2);
    EXPECT_EQ(bucket1.slot_index(), entry1.slot_index);
    EXPECT_EQ(bucket2.slot_index(), entry2.slot_index);
    EXPECT_FALSE(index.find(make_ip_key(3)));
}

TEST(Http1ConnectionBucketIndexTest, EraseKeepsLaterCollisionsReachableAndUpdatesSlotIndices) {
    Http1ConnectionPoolGroupBucket bucket_a;
    Http1ConnectionPoolGroupBucket bucket_b;
    Http1ConnectionPoolGroupBucket bucket_c;
    Http1ConnectionBucketIndex index;
    ASSERT_TRUE(index.init(4));

    const auto octets = find_colliding_ip_suffixes(index.slot_capacity());
    const auto key_a = make_ip_key(octets[0]);
    const auto key_b = make_ip_key(octets[1]);
    const auto key_c = make_ip_key(octets[2]);

    ASSERT_EQ(index.insert(key_a, bucket_a), IoErr::None);
    ASSERT_EQ(index.insert(key_b, bucket_b), IoErr::None);
    ASSERT_EQ(index.insert(key_c, bucket_c), IoErr::None);

    const std::uint32_t removed_slot = bucket_a.slot_index();
    index.erase(removed_slot);

    EXPECT_EQ(bucket_a.slot_index(), Http1ConnectionPoolGroupBucket::kInvalidSlotIndex);

    auto entry_b = index.find(key_b);
    auto entry_c = index.find(key_c);
    ASSERT_TRUE(entry_b);
    ASSERT_TRUE(entry_c);
    EXPECT_EQ(entry_b.bucket, &bucket_b);
    EXPECT_EQ(entry_c.bucket, &bucket_c);
    EXPECT_EQ(entry_b.slot_index, bucket_b.slot_index());
    EXPECT_EQ(entry_c.slot_index, bucket_c.slot_index());
    EXPECT_LT(bucket_b.slot_index(), index.slot_capacity());
    EXPECT_LT(bucket_c.slot_index(), index.slot_capacity());
    EXPECT_EQ(index.size(), 2u);
}

TEST(Http1ConnectionBucketIndexTest, GrowsAndRewritesBucketSlotIndices) {
    std::array<Http1ConnectionPoolGroupBucket, 5> buckets{};
    Http1ConnectionBucketIndex index;
    ASSERT_TRUE(index.init(0));
    EXPECT_EQ(index.slot_capacity(), 0u);

    const auto key1 = make_ip_key(1);
    const auto key2 = make_ip_key(2);
    const auto key3 = make_ip_key(3);
    const auto key4 = make_ip_key(4);
    const auto key5 = make_ip_key(5);

    ASSERT_EQ(index.insert(key1, buckets[0]), IoErr::None);
    ASSERT_EQ(index.insert(key2, buckets[1]), IoErr::None);
    ASSERT_EQ(index.insert(key3, buckets[2]), IoErr::None);
    ASSERT_EQ(index.insert(key4, buckets[3]), IoErr::None);
    EXPECT_EQ(index.slot_capacity(), 8u);

    ASSERT_EQ(index.insert(key5, buckets[4]), IoErr::None);
    EXPECT_EQ(index.slot_capacity(), 16u);
    EXPECT_EQ(index.size(), 5u);

    const std::array<Http1ConnectionGroupKey, 5> keys = {key1, key2, key3, key4, key5};
    for (std::size_t i = 0; i < keys.size(); ++i) {
        auto entry = index.find(keys[i]);
        ASSERT_TRUE(entry);
        EXPECT_EQ(entry.bucket, &buckets[i]);
        EXPECT_EQ(entry.slot_index, buckets[i].slot_index());
    }
}
