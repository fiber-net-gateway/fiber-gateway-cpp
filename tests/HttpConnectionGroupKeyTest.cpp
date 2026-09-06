#include <gtest/gtest.h>

#include <functional>
#include <string>

#include <fiber/http/HttpConnectionGroupKey.h>

namespace {

using fiber::http::HttpConnectionGroupKey;
using fiber::http::HttpConnectionPoolAffinity;

TEST(HttpConnectionGroupKeyTest, NameKeyNormalizesCaseAndCachesHash) {
    auto left = HttpConnectionGroupKey::from_name("Example.COM", 443, HttpConnectionGroupKey::Scheme::Https);
    auto right = HttpConnectionGroupKey::from_name("example.com", 443, HttpConnectionGroupKey::Scheme::Https);

    ASSERT_TRUE(left.has_value());
    ASSERT_TRUE(right.has_value());
    EXPECT_EQ(left->host_kind(), HttpConnectionGroupKey::HostKind::Name);
    EXPECT_EQ(left->scheme(), HttpConnectionGroupKey::Scheme::Https);
    EXPECT_EQ(left->port(), 443);
    EXPECT_EQ(left->host_name(), "example.com");
    EXPECT_EQ(left->hash(), right->hash());
    EXPECT_EQ(*left, *right);
    EXPECT_EQ(std::hash<HttpConnectionGroupKey>{}(*left), std::hash<HttpConnectionGroupKey>{}(*right));
}

TEST(HttpConnectionGroupKeyTest, NameKeysDifferentSchemeOrPortDoNotMatch) {
    auto http_key = HttpConnectionGroupKey::from_name("example.com", 80, HttpConnectionGroupKey::Scheme::Http);
    auto https_key = HttpConnectionGroupKey::from_name("example.com", 80, HttpConnectionGroupKey::Scheme::Https);
    auto other_port = HttpConnectionGroupKey::from_name("example.com", 8080, HttpConnectionGroupKey::Scheme::Http);

    ASSERT_TRUE(http_key.has_value());
    ASSERT_TRUE(https_key.has_value());
    ASSERT_TRUE(other_port.has_value());
    EXPECT_NE(*http_key, *https_key);
    EXPECT_NE(*http_key, *other_port);
}

TEST(HttpConnectionGroupKeyTest, IpKeyUsesAddressValue) {
    const auto left = HttpConnectionGroupKey::from_ip(fiber::net::IpAddress::v4({127, 0, 0, 1}), 8080,
                                                      HttpConnectionGroupKey::Scheme::Http);
    const auto right = HttpConnectionGroupKey::from_ip(fiber::net::IpAddress::v4({127, 0, 0, 1}), 8080,
                                                       HttpConnectionGroupKey::Scheme::Http);
    const auto other_ip = HttpConnectionGroupKey::from_ip(fiber::net::IpAddress::v4({127, 0, 0, 2}), 8080,
                                                          HttpConnectionGroupKey::Scheme::Http);

    EXPECT_EQ(left.host_kind(), HttpConnectionGroupKey::HostKind::Ip);
    EXPECT_EQ(left.ip_address().v4_bytes(), right.ip_address().v4_bytes());
    EXPECT_EQ(left, right);
    EXPECT_EQ(left.hash(), right.hash());
    EXPECT_NE(left, other_ip);
}

TEST(HttpConnectionGroupKeyTest, NameFactoryRejectsInvalidHostLength) {
    EXPECT_FALSE(HttpConnectionGroupKey::from_name({}, 80, HttpConnectionGroupKey::Scheme::Http).has_value());

    constexpr std::size_t too_long_size = HttpConnectionGroupKey::kMaxHostNameSize + 1;
    std::string host(too_long_size, 'a');
    EXPECT_FALSE(HttpConnectionGroupKey::from_name(host, 80, HttpConnectionGroupKey::Scheme::Http).has_value());
}

TEST(HttpConnectionGroupKeyTest, PoolAffinityPartitionsOtherwiseIdenticalEndpoints) {
    constexpr HttpConnectionPoolAffinity first_identity{101};
    constexpr HttpConnectionPoolAffinity second_identity{202};
    auto first = HttpConnectionGroupKey::from_name("example.com", 443, HttpConnectionGroupKey::Scheme::Https,
                                                   first_identity);
    auto same = HttpConnectionGroupKey::from_name("EXAMPLE.COM", 443, HttpConnectionGroupKey::Scheme::Https,
                                                  first_identity);
    auto second = HttpConnectionGroupKey::from_name("example.com", 443, HttpConnectionGroupKey::Scheme::Https,
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

TEST(HttpConnectionGroupKeyTest, DefaultAffinityPreservesLegacyGrouping) {
    const auto implicit = HttpConnectionGroupKey::from_ip(fiber::net::IpAddress::loopback_v4(), 443,
                                                          HttpConnectionGroupKey::Scheme::Https);
    const auto explicit_zero =
            HttpConnectionGroupKey::from_ip(fiber::net::IpAddress::loopback_v4(), 443,
                                            HttpConnectionGroupKey::Scheme::Https, HttpConnectionPoolAffinity{0});

    EXPECT_EQ(implicit.pool_affinity(), HttpConnectionPoolAffinity{});
    EXPECT_EQ(implicit, explicit_zero);
    EXPECT_EQ(implicit.hash(), explicit_zero.hash());
}

} // namespace
