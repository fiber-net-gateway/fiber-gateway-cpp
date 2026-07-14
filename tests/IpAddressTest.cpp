#include <gtest/gtest.h>

#include <type_traits>

#include "net/IpAddress.h"

static_assert(std::is_trivially_default_constructible_v<fiber::net::IpAddress>);
static_assert(std::is_trivially_copyable_v<fiber::net::IpAddress>);
static_assert(std::is_trivially_destructible_v<fiber::net::IpAddress>);
static_assert(std::is_standard_layout_v<fiber::net::IpAddress>);

TEST(IpAddressTest, ValueInitializationProducesAnyIpv4) {
    fiber::net::IpAddress address{};
    EXPECT_TRUE(address.is_v4());
    EXPECT_TRUE(address.is_unspecified());
    EXPECT_EQ(address.scope_id(), 0u);
    EXPECT_EQ(address, fiber::net::IpAddress::any_v4());
}

TEST(IpAddressTest, ParsesWithoutChangingOutputOnFailure) {
    fiber::net::IpAddress address = fiber::net::IpAddress::loopback_v4();
    ASSERT_TRUE(fiber::net::IpAddress::parse("[2001:db8::1]", address));
    EXPECT_TRUE(address.is_v6());
    EXPECT_EQ(address.to_string(), "2001:db8::1");

    const fiber::net::IpAddress previous = address;
    EXPECT_FALSE(fiber::net::IpAddress::parse("not-an-address", address));
    EXPECT_EQ(address, previous);
}

TEST(IpAddressTest, LoopbackDetection) {
    auto v4 = fiber::net::IpAddress::loopback_v4();
    EXPECT_TRUE(v4.is_loopback());
    EXPECT_FALSE(v4.is_unspecified());
    EXPECT_FALSE(v4.is_multicast());

    auto v6 = fiber::net::IpAddress::loopback_v6();
    EXPECT_TRUE(v6.is_loopback());
    EXPECT_FALSE(v6.is_unspecified());
    EXPECT_FALSE(v6.is_multicast());

    auto v4_other = fiber::net::IpAddress::v4({192, 168, 0, 1});
    EXPECT_FALSE(v4_other.is_loopback());
}

TEST(IpAddressTest, UnspecifiedDetection) {
    auto v4 = fiber::net::IpAddress::any_v4();
    EXPECT_TRUE(v4.is_unspecified());
    EXPECT_FALSE(v4.is_loopback());

    auto v6 = fiber::net::IpAddress::any_v6();
    EXPECT_TRUE(v6.is_unspecified());
    EXPECT_FALSE(v6.is_loopback());
}

TEST(IpAddressTest, MulticastDetection) {
    auto v4 = fiber::net::IpAddress::v4({239, 1, 2, 3});
    EXPECT_TRUE(v4.is_multicast());

    std::array<std::uint8_t, 16> bytes{};
    bytes[0] = 0xFF;
    auto v6 = fiber::net::IpAddress::v6(bytes);
    EXPECT_TRUE(v6.is_multicast());

    auto v4_other = fiber::net::IpAddress::v4({10, 0, 0, 1});
    EXPECT_FALSE(v4_other.is_multicast());
}
