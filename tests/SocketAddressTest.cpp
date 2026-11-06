#include <gtest/gtest.h>

#include <array>
#include <string_view>

#include <fiber/net/SocketAddress.h>

TEST(SocketAddressTest, ToStringViewMatchesToString) {
    const std::array<fiber::net::SocketAddress, 4> addresses = {
            fiber::net::SocketAddress(fiber::net::IpAddress::v4({192, 168, 0, 1}), 8080),
            fiber::net::SocketAddress(fiber::net::IpAddress::any_v4(), 0),
            fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v6(), 443),
            fiber::net::SocketAddress(fiber::net::IpAddress::any_v6(), 65535),
    };
    for (const auto &address: addresses) {
        std::array<char, fiber::net::SocketAddress::kMaxTextLen> buffer{};
        const std::string_view text = address.to_string_view(buffer);
        EXPECT_EQ(text, address.to_string());
        EXPECT_EQ(buffer[text.size()], '\0');
    }
}

TEST(SocketAddressTest, ToStringViewFormatsV4AndV6) {
    std::array<char, fiber::net::SocketAddress::kMaxTextLen> buffer{};
    const fiber::net::SocketAddress v4(fiber::net::IpAddress::v4({10, 0, 0, 1}), 3128);
    EXPECT_EQ(v4.to_string_view(buffer), "10.0.0.1:3128");

    const fiber::net::SocketAddress v6(fiber::net::IpAddress::loopback_v6(), 80);
    EXPECT_EQ(v6.to_string_view(buffer), "[::1]:80");
    EXPECT_STREQ(buffer.data(), "[::1]:80");

    const fiber::net::SocketAddress any_v6(fiber::net::IpAddress::any_v6(), 65535);
    EXPECT_EQ(any_v6.to_string_view(buffer), "[::]:65535");
}
