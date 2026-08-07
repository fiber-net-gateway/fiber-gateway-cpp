#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include <fiber/net/IpAddress.h>
#include <fiber/net/SocketAddress.h>
#include <fiber/quic/QuicConnectionId.h>
#include <fiber/quic/QuicToken.h>

namespace {

fiber::net::SocketAddress v4_addr(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d, std::uint16_t port) {
    return {fiber::net::IpAddress::v4({a, b, c, d}), port};
}

fiber::quic::QuicConnectionId cid_from(std::initializer_list<std::uint8_t> bytes) {
    auto cid = fiber::quic::QuicConnectionId::from_bytes(bytes.begin(), bytes.size());
    EXPECT_TRUE(cid.has_value());
    return cid.value_or(fiber::quic::QuicConnectionId{});
}

std::array<std::uint8_t, fiber::quic::kQuicAddressValidationKeyLength> test_key() {
    std::array<std::uint8_t, fiber::quic::kQuicAddressValidationKeyLength> key{};
    for (std::size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<std::uint8_t>(0x30U + i);
    }
    return key;
}

} // namespace

TEST(QuicTokenTest, RetryTokenRoundTripsAndBindsPeerPort) {
    const auto key = test_key();
    const auto peer = v4_addr(127, 0, 0, 1, 4433);
    const auto other_port = v4_addr(127, 0, 0, 1, 4434);
    const auto odcid = cid_from({0x83, 0x94, 0xc8, 0xf0});

    auto token =
            fiber::quic::quic_create_address_token(key, peer, 2000, fiber::quic::QuicAddressTokenKind::Retry, &odcid);
    ASSERT_TRUE(token.has_value()) << static_cast<int>(token.error());

    auto valid = fiber::quic::quic_validate_address_token(key, peer, 1000, token->slice());
    ASSERT_TRUE(valid.has_value()) << static_cast<int>(valid.error());
    EXPECT_EQ(valid->status, fiber::quic::QuicAddressTokenValidationStatus::Valid);
    EXPECT_EQ(valid->kind, fiber::quic::QuicAddressTokenKind::Retry);
    EXPECT_EQ(valid->original_destination_connection_id.size(), odcid.size());

    auto wrong_port = fiber::quic::quic_validate_address_token(key, other_port, 1000, token->slice());
    ASSERT_TRUE(wrong_port.has_value()) << static_cast<int>(wrong_port.error());
    EXPECT_EQ(wrong_port->status, fiber::quic::QuicAddressTokenValidationStatus::Invalid);
    EXPECT_EQ(wrong_port->kind, fiber::quic::QuicAddressTokenKind::Retry);

    auto expired = fiber::quic::quic_validate_address_token(key, peer, 2001, token->slice());
    ASSERT_TRUE(expired.has_value()) << static_cast<int>(expired.error());
    EXPECT_EQ(expired->status, fiber::quic::QuicAddressTokenValidationStatus::Expired);
}

TEST(QuicTokenTest, NewTokenBindsPeerIpButNotPort) {
    const auto key = test_key();
    const auto peer = v4_addr(127, 0, 0, 1, 4433);
    const auto other_port = v4_addr(127, 0, 0, 1, 4434);
    const auto other_ip = v4_addr(127, 0, 0, 2, 4433);

    auto token = fiber::quic::quic_create_address_token(key, peer, 2000, fiber::quic::QuicAddressTokenKind::NewToken,
                                                        nullptr);
    ASSERT_TRUE(token.has_value()) << static_cast<int>(token.error());

    auto same_ip = fiber::quic::quic_validate_address_token(key, other_port, 1000, token->slice());
    ASSERT_TRUE(same_ip.has_value()) << static_cast<int>(same_ip.error());
    EXPECT_EQ(same_ip->status, fiber::quic::QuicAddressTokenValidationStatus::Valid);
    EXPECT_EQ(same_ip->kind, fiber::quic::QuicAddressTokenKind::NewToken);

    auto wrong_ip = fiber::quic::quic_validate_address_token(key, other_ip, 1000, token->slice());
    ASSERT_TRUE(wrong_ip.has_value()) << static_cast<int>(wrong_ip.error());
    EXPECT_EQ(wrong_ip->status, fiber::quic::QuicAddressTokenValidationStatus::Invalid);
    EXPECT_EQ(wrong_ip->kind, fiber::quic::QuicAddressTokenKind::NewToken);
}

TEST(QuicTokenTest, TamperedTokenIsGarbage) {
    const auto key = test_key();
    const auto peer = v4_addr(127, 0, 0, 1, 4433);

    auto token = fiber::quic::quic_create_address_token(key, peer, 2000, fiber::quic::QuicAddressTokenKind::NewToken,
                                                        nullptr);
    ASSERT_TRUE(token.has_value()) << static_cast<int>(token.error());
    ASSERT_GT(token->len, 0U);
    token->bytes[0] ^= 0x01U;

    auto checked = fiber::quic::quic_validate_address_token(key, peer, 1000, token->slice());
    ASSERT_TRUE(checked.has_value()) << static_cast<int>(checked.error());
    EXPECT_EQ(checked->status, fiber::quic::QuicAddressTokenValidationStatus::Garbage);
}
