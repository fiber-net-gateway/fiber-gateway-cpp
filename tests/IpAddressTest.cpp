#include <gtest/gtest.h>

#include "net/IpAddress.h"

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
