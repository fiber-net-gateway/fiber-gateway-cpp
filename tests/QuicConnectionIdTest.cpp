#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <type_traits>

#include <fiber/quic/QuicConnectionId.h>

namespace {

using fiber::quic::QuicConnectionId;

static_assert(std::is_trivially_copyable_v<QuicConnectionId>);
static_assert(std::is_standard_layout_v<fiber::quic::QuicConnectionIdIndex>);

TEST(QuicConnectionIdTest, EmptyConstructionAndResetHaveTheSameHash) {
    const QuicConnectionId empty{};
    const auto parsed = QuicConnectionId::from_bytes(nullptr, 0);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->empty());
    EXPECT_EQ(parsed->hash(), empty.hash());

    const std::array<std::uint8_t, 1> bytes{0x42};
    auto populated = QuicConnectionId::from_bytes(bytes.data(), bytes.size());
    ASSERT_TRUE(populated.has_value());
    *populated = {};
    EXPECT_TRUE(populated->empty());
    EXPECT_EQ(populated->hash(), empty.hash());
}

TEST(QuicConnectionIdTest, CopiesOwnTheirBytesAndPreserveTheHash) {
    std::array<std::uint8_t, fiber::quic::kMaxConnectionIdLength> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::uint8_t>(i + 1);
    }
    const auto first = QuicConnectionId::from_bytes(bytes.data(), bytes.size());
    const auto second = QuicConnectionId::from_bytes(bytes.data(), bytes.size());
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    const QuicConnectionId copy = *first;
    QuicConnectionId assigned{};
    assigned = *first;
    bytes.fill(0);

    EXPECT_EQ(first->size(), bytes.size());
    EXPECT_EQ(first->data()[0], 1);
    EXPECT_EQ(first->data()[bytes.size() - 1], bytes.size());
    EXPECT_EQ(first->hash(), second->hash());
    EXPECT_EQ(copy.hash(), first->hash());
    EXPECT_EQ(assigned.hash(), first->hash());
    EXPECT_EQ(std::memcmp(copy.data(), first->data(), first->size()), 0);
    EXPECT_EQ(std::memcmp(assigned.data(), first->data(), first->size()), 0);
}

TEST(QuicConnectionIdTest, HashIncludesLengthAndEmbeddedZeros) {
    const std::array<std::uint8_t, 3> bytes{1, 0, 2};
    const auto prefix = QuicConnectionId::from_bytes(bytes.data(), 1);
    const auto with_zero = QuicConnectionId::from_bytes(bytes.data(), 2);
    const auto full = QuicConnectionId::from_bytes(bytes.data(), bytes.size());
    ASSERT_TRUE(prefix.has_value());
    ASSERT_TRUE(with_zero.has_value());
    ASSERT_TRUE(full.has_value());
    EXPECT_NE(prefix->hash(), with_zero->hash());
    EXPECT_NE(with_zero->hash(), full->hash());
    EXPECT_NE(prefix->hash(), full->hash());
}

TEST(QuicConnectionIdTest, RejectsInvalidBytes) {
    const std::array<std::uint8_t, fiber::quic::kMaxConnectionIdLength + 1> bytes{};
    EXPECT_FALSE(QuicConnectionId::from_bytes(nullptr, 1).has_value());
    EXPECT_FALSE(QuicConnectionId::from_bytes(bytes.data(), bytes.size()).has_value());
}

} // namespace
