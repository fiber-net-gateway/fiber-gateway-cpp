#include <gtest/gtest.h>

#include <functional>
#include <string>

#include <fiber/http/Http1ConnectionGroupKey.h>

namespace {

using fiber::http::Http1ConnectionGroupKey;
using fiber::http::Http1ConnectionPoolAffinity;

TEST(Http1ConnectionGroupKeyTest, NameKeyNormalizesCaseAndCachesHash) {
    auto left = Http1ConnectionGroupKey::from_name("Example.COM", 443, Http1ConnectionGroupKey::Scheme::Https);
    auto right = Http1ConnectionGroupKey::from_name("example.com", 443, Http1ConnectionGroupKey::Scheme::Https);

    ASSERT_TRUE(left.has_value());
    ASSERT_TRUE(right.has_value());
    EXPECT_EQ(left->host_kind(), Http1ConnectionGroupKey::HostKind::Name);
    EXPECT_EQ(left->scheme(), Http1ConnectionGroupKey::Scheme::Https);
    EXPECT_EQ(left->port(), 443);
    EXPECT_EQ(left->host_name(), "example.com");
    EXPECT_EQ(left->hash(), right->hash());
    EXPECT_EQ(*left, *right);
    EXPECT_EQ(std::hash<Http1ConnectionGroupKey>{}(*left), std::hash<Http1ConnectionGroupKey>{}(*right));
}

TEST(Http1ConnectionGroupKeyTest, NameKeysDifferentSchemeOrPortDoNotMatch) {
    auto http_key = Http1ConnectionGroupKey::from_name("example.com", 80, Http1ConnectionGroupKey::Scheme::Http);
    auto https_key = Http1ConnectionGroupKey::from_name("example.com", 80, Http1ConnectionGroupKey::Scheme::Https);
    auto other_port = Http1ConnectionGroupKey::from_name("example.com", 8080, Http1ConnectionGroupKey::Scheme::Http);

    ASSERT_TRUE(http_key.has_value());
    ASSERT_TRUE(https_key.has_value());
    ASSERT_TRUE(other_port.has_value());
    EXPECT_NE(*http_key, *https_key);
    EXPECT_NE(*http_key, *other_port);
}

TEST(Http1ConnectionGroupKeyTest, IpKeyUsesAddressValue) {
    const auto left = Http1ConnectionGroupKey::from_ip(fiber::net::IpAddress::v4({127, 0, 0, 1}), 8080,
                                                       Http1ConnectionGroupKey::Scheme::Http);
    const auto right = Http1ConnectionGroupKey::from_ip(fiber::net::IpAddress::v4({127, 0, 0, 1}), 8080,
                                                        Http1ConnectionGroupKey::Scheme::Http);
    const auto other_ip = Http1ConnectionGroupKey::from_ip(fiber::net::IpAddress::v4({127, 0, 0, 2}), 8080,
                                                           Http1ConnectionGroupKey::Scheme::Http);

    EXPECT_EQ(left.host_kind(), Http1ConnectionGroupKey::HostKind::Ip);
    EXPECT_EQ(left.ip_address().v4_bytes(), right.ip_address().v4_bytes());
    EXPECT_EQ(left, right);
    EXPECT_EQ(left.hash(), right.hash());
    EXPECT_NE(left, other_ip);
}

TEST(Http1ConnectionGroupKeyTest, NameFactoryRejectsInvalidHostLength) {
    EXPECT_FALSE(Http1ConnectionGroupKey::from_name({}, 80, Http1ConnectionGroupKey::Scheme::Http).has_value());

    constexpr std::size_t too_long_size = Http1ConnectionGroupKey::kMaxHostNameSize + 1;
    std::string host(too_long_size, 'a');
    EXPECT_FALSE(Http1ConnectionGroupKey::from_name(host, 80, Http1ConnectionGroupKey::Scheme::Http).has_value());
}

TEST(Http1ConnectionGroupKeyTest, PoolAffinityPartitionsOtherwiseIdenticalEndpoints) {
    constexpr Http1ConnectionPoolAffinity first_identity{101};
    constexpr Http1ConnectionPoolAffinity second_identity{202};
    auto first = Http1ConnectionGroupKey::from_name("example.com", 443, Http1ConnectionGroupKey::Scheme::Https,
                                                    first_identity);
    auto same = Http1ConnectionGroupKey::from_name("EXAMPLE.COM", 443, Http1ConnectionGroupKey::Scheme::Https,
                                                   first_identity);
    auto second = Http1ConnectionGroupKey::from_name("example.com", 443, Http1ConnectionGroupKey::Scheme::Https,
                                                     second_identity);

    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(same.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(first->pool_affinity(), first_identity);
    EXPECT_EQ(*first, *same);
    EXPECT_EQ(first->hash(), same->hash());
    EXPECT_NE(*first, *second);
    EXPECT_NE(first->hash(), second->hash());
}

TEST(Http1ConnectionGroupKeyTest, DefaultAffinityPreservesLegacyGrouping) {
    const auto implicit = Http1ConnectionGroupKey::from_ip(fiber::net::IpAddress::loopback_v4(), 443,
                                                           Http1ConnectionGroupKey::Scheme::Https);
    const auto explicit_zero =
            Http1ConnectionGroupKey::from_ip(fiber::net::IpAddress::loopback_v4(), 443,
                                             Http1ConnectionGroupKey::Scheme::Https, Http1ConnectionPoolAffinity{0});

    EXPECT_EQ(implicit.pool_affinity(), Http1ConnectionPoolAffinity{});
    EXPECT_EQ(implicit, explicit_zero);
    EXPECT_EQ(implicit.hash(), explicit_zero.hash());
}

} // namespace
