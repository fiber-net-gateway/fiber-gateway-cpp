#include <gtest/gtest.h>

#include <type_traits>

#include <fiber/net/IpAddress.h>

static_assert(std::is_trivially_default_constructible_v<fiber::net::IpAddress>);
static_assert(std::is_trivially_copyable_v<fiber::net::IpAddress>);
static_assert(std::is_trivially_destructible_v<fiber::net::IpAddress>);
static_assert(std::is_standard_layout_v<fiber::net::IpAddress>);

TEST(IpAddressTest, ValueInitializationProducesAnyIpv4) {
    fiber::net::IpAddress address{};
    EXPECT_TRUE(address.is_v4());
    EXPECT_TRUE(address.is_unspecified());
    EXPECT_EQ(address.scope_id(), 0U);
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

TEST(IpAddressTest, ToStringViewMatchesToString) {
    const std::array<fiber::net::IpAddress, 5> addresses = {
            fiber::net::IpAddress::v4({192, 168, 0, 1}),
            fiber::net::IpAddress::v4({255, 255, 255, 255}),
            fiber::net::IpAddress::any_v4(),
            fiber::net::IpAddress::loopback_v6(),
            fiber::net::IpAddress::v6({0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}),
    };
    for (const auto &address: addresses) {
        std::array<char, fiber::net::IpAddress::kMaxTextLen> buffer{};
        const std::string_view text = address.to_string_view(buffer);
        EXPECT_EQ(text, address.to_string());
        EXPECT_EQ(buffer[text.size()], '\0');
    }
}

TEST(IpAddressTest, ToStringViewBorrowsCallerBuffer) {
    std::array<char, fiber::net::IpAddress::kMaxTextLen> buffer{};
    const fiber::net::IpAddress address = fiber::net::IpAddress::loopback_v6();
    const std::string_view text = address.to_string_view(buffer);
    EXPECT_EQ(text, "::1");
    // The view points into the caller buffer and the text stays NUL-terminated.
    EXPECT_EQ(text.data(), buffer.data());
    EXPECT_STREQ(buffer.data(), "::1");
}
