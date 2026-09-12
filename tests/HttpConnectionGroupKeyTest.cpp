#include <gtest/gtest.h>

#include <functional>
#include <string>

#include <fiber/http/HttpConnectionGroupKey.h>

namespace {

using fiber::http::HttpConnectionGroupKey;
using Scheme = HttpConnectionGroupKey::Scheme;

const fiber::net::IpAddress kLoopback = fiber::net::IpAddress::loopback_v4();
const fiber::net::IpAddress kOtherAddress = fiber::net::IpAddress::v4({127, 0, 0, 2});

TEST(HttpConnectionGroupKeyTest, NameKeyNormalizesCaseAndCachesHash) {
    auto left = HttpConnectionGroupKey::make("Example.COM", 443, Scheme::Https);
    auto right = HttpConnectionGroupKey::make("example.com", 443, Scheme::Https);

    ASSERT_TRUE(left.has_value());
    ASSERT_TRUE(right.has_value());
    EXPECT_EQ(left->scheme(), Scheme::Https);
    EXPECT_EQ(left->port(), 443);
    EXPECT_EQ(left->host(), "example.com");
    EXPECT_FALSE(left->has_ip());
    EXPECT_EQ(left->hash(), right->hash());
    EXPECT_EQ(*left, *right);
    EXPECT_EQ(std::hash<HttpConnectionGroupKey>{}(*left), std::hash<HttpConnectionGroupKey>{}(*right));
}

TEST(HttpConnectionGroupKeyTest, NameKeysDifferentSchemeOrPortDoNotMatch) {
    auto http_key = HttpConnectionGroupKey::make("example.com", 80, Scheme::Http);
    auto https_key = HttpConnectionGroupKey::make("example.com", 80, Scheme::Https);
    auto other_port = HttpConnectionGroupKey::make("example.com", 8080, Scheme::Http);

    ASSERT_TRUE(http_key.has_value());
    ASSERT_TRUE(https_key.has_value());
    ASSERT_TRUE(other_port.has_value());
    EXPECT_NE(*http_key, *https_key);
    EXPECT_NE(*http_key, *other_port);
}

TEST(HttpConnectionGroupKeyTest, LiteralHostParsesIntoAddress) {
    const auto v4 = HttpConnectionGroupKey::make("127.0.0.1", 8080, Scheme::Http);
    const auto same = HttpConnectionGroupKey::make("127.0.0.1", 8080, Scheme::Http);
    const auto other_ip = HttpConnectionGroupKey::make("127.0.0.2", 8080, Scheme::Http);
    const auto v6 = HttpConnectionGroupKey::make("::1", 8080, Scheme::Http);

    ASSERT_TRUE(v4.has_value());
    ASSERT_TRUE(same.has_value());
    ASSERT_TRUE(other_ip.has_value());
    ASSERT_TRUE(v6.has_value());
    EXPECT_EQ(v4->host(), "127.0.0.1");
    ASSERT_TRUE(v4->has_ip());
    EXPECT_EQ(v4->ip(), kLoopback);
    EXPECT_EQ(*v4, *same);
    EXPECT_EQ(v4->hash(), same->hash());
    EXPECT_NE(*v4, *other_ip);
    ASSERT_TRUE(v6->has_ip());
    EXPECT_EQ(v6->ip(), fiber::net::IpAddress::loopback_v6());
}

TEST(HttpConnectionGroupKeyTest, PinnedAddressIsPartOfIdentity) {
    auto resolved = HttpConnectionGroupKey::make("example.com", 443, Scheme::Https);
    auto pinned = HttpConnectionGroupKey::make("EXAMPLE.com", 443, Scheme::Https, kLoopback);
    auto pinned_same = HttpConnectionGroupKey::make("example.com", 443, Scheme::Https, kLoopback);
    auto pinned_other = HttpConnectionGroupKey::make("example.com", 443, Scheme::Https, kOtherAddress);

    ASSERT_TRUE(resolved.has_value());
    ASSERT_TRUE(pinned.has_value());
    ASSERT_TRUE(pinned_same.has_value());
    ASSERT_TRUE(pinned_other.has_value());
    EXPECT_EQ(pinned->host(), "example.com");
    ASSERT_TRUE(pinned->has_ip());
    EXPECT_EQ(pinned->ip(), kLoopback);
    EXPECT_EQ(*pinned, *pinned_same);
    EXPECT_EQ(pinned->hash(), pinned_same->hash());
    EXPECT_NE(*pinned, *resolved);
    EXPECT_NE(*pinned, *pinned_other);
}

TEST(HttpConnectionGroupKeyTest, DifferentNamesPinnedToSameAddressStayDistinct) {
    auto first = HttpConnectionGroupKey::make("first.example", 443, Scheme::Https, kLoopback);
    auto second = HttpConnectionGroupKey::make("second.example", 443, Scheme::Https, kLoopback);
    auto literal = HttpConnectionGroupKey::make("127.0.0.1", 443, Scheme::Http);
    auto pinned_http = HttpConnectionGroupKey::make("first.example", 443, Scheme::Http, kLoopback);

    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    ASSERT_TRUE(literal.has_value());
    ASSERT_TRUE(pinned_http.has_value());
    EXPECT_NE(*first, *second);
    EXPECT_NE(*literal, *pinned_http);
}

TEST(HttpConnectionGroupKeyTest, HttpsRejectsLiteralHost) {
    EXPECT_FALSE(HttpConnectionGroupKey::make("127.0.0.1", 443, Scheme::Https).has_value());
    EXPECT_FALSE(HttpConnectionGroupKey::make("::1", 443, Scheme::Https).has_value());
    EXPECT_TRUE(HttpConnectionGroupKey::make("127.0.0.1", 443, Scheme::Http).has_value());
}

TEST(HttpConnectionGroupKeyTest, LiteralHostRejectsExplicitAddress) {
    EXPECT_FALSE(HttpConnectionGroupKey::make("127.0.0.1", 80, Scheme::Http, kLoopback).has_value());
    EXPECT_FALSE(HttpConnectionGroupKey::make("127.0.0.1", 80, Scheme::Http, kOtherAddress).has_value());
}

TEST(HttpConnectionGroupKeyTest, RejectsMalformedHosts) {
    EXPECT_FALSE(HttpConnectionGroupKey::make({}, 80, Scheme::Http).has_value());
    EXPECT_FALSE(HttpConnectionGroupKey::make("[::1]", 80, Scheme::Http).has_value());
    EXPECT_FALSE(HttpConnectionGroupKey::make("example.com:80", 80, Scheme::Http).has_value());
    EXPECT_FALSE(HttpConnectionGroupKey::make("example.com/path", 80, Scheme::Http).has_value());
    EXPECT_FALSE(HttpConnectionGroupKey::make("exam ple.com", 80, Scheme::Http).has_value());

    const std::string too_long(HttpConnectionGroupKey::kMaxHostSize + 1, 'a');
    EXPECT_FALSE(HttpConnectionGroupKey::make(too_long, 80, Scheme::Http).has_value());
    const std::string max_size(HttpConnectionGroupKey::kMaxHostSize, 'a');
    EXPECT_TRUE(HttpConnectionGroupKey::make(max_size, 80, Scheme::Http).has_value());
}

} // namespace
