#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>

#include <fiber/dns/DnsMessage.h>
#include <fiber/dns/DnsName.h>
#include "dns/detail/DnsQuerySecurity.h"

namespace {

using fiber::common::IoErr;
using fiber::dns::QueryOptions;
using fiber::dns::QuestionSpec;
using fiber::dns::RecordClass;
using fiber::dns::RecordType;

constexpr std::uint16_t kInvalidMapping = 0xffffU;

void write_be16(std::uint8_t *dst, std::uint16_t value) {
    dst[0] = static_cast<std::uint8_t>(value >> 8U);
    dst[1] = static_cast<std::uint8_t>(value & 0xffU);
}

std::size_t encode_test_query(std::array<std::uint8_t, 512> &packet, std::uint16_t id, std::string_view name,
                              std::uint16_t type = static_cast<std::uint16_t>(RecordType::A),
                              std::uint16_t dns_class = static_cast<std::uint16_t>(RecordClass::IN)) {
    QueryOptions options;
    options.id = id;
    options.use_edns = false;
    QuestionSpec question{name, type, dns_class};
    auto encoded = fiber::dns::encode_query(options, question, packet.data(), packet.size());
    EXPECT_TRUE(encoded.has_value());
    return encoded.value_or(0);
}

TEST(DnsQuerySecurityTest, QueryIdProbeFindsCollisionAndWraps) {
    auto mapping = std::make_unique<std::uint16_t[]>(fiber::dns::detail::kDnsQueryIdCount);
    std::fill_n(mapping.get(), fiber::dns::detail::kDnsQueryIdCount, kInvalidMapping);
    mapping[0xffffU] = 7;

    auto selected = fiber::dns::detail::select_query_id(mapping.get(), kInvalidMapping, 0xffffU, 0);

    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(*selected, 0U);
}

TEST(DnsQuerySecurityTest, QueryIdProbeCoversEntireSpaceWithOddStride) {
    auto mapping = std::make_unique<std::uint16_t[]>(fiber::dns::detail::kDnsQueryIdCount);
    std::fill_n(mapping.get(), fiber::dns::detail::kDnsQueryIdCount, 1U);

    constexpr std::uint16_t start = 1234;
    constexpr std::uint16_t stride = 0x8000U;
    std::uint16_t last = start;
    for (std::size_t i = 1; i < fiber::dns::detail::kDnsQueryIdCount; ++i) {
        last = static_cast<std::uint16_t>(last + static_cast<std::uint16_t>(stride | 1U));
    }
    mapping[last] = kInvalidMapping;

    auto selected = fiber::dns::detail::select_query_id(mapping.get(), kInvalidMapping, start, stride);

    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(*selected, last);
}

TEST(DnsQuerySecurityTest, QueryIdProbeReturnsBusyWhenFull) {
    auto mapping = std::make_unique<std::uint16_t[]>(fiber::dns::detail::kDnsQueryIdCount);
    std::fill_n(mapping.get(), fiber::dns::detail::kDnsQueryIdCount, 1U);

    auto selected = fiber::dns::detail::select_query_id(mapping.get(), kInvalidMapping, 1, 4);

    ASSERT_FALSE(selected.has_value());
    EXPECT_EQ(selected.error(), IoErr::Busy);
}

TEST(DnsQuerySecurityTest, Applies0x20OnlyToAsciiLetters) {
    std::array<std::uint8_t, 512> packet{};
    const std::size_t packet_len = encode_test_query(packet, 9, "Ab-1.Example");
    std::array<std::uint8_t, fiber::dns::detail::kDns0x20RandomBytes> random_bits{};
    random_bits.fill(0xffU);

    ASSERT_EQ(fiber::dns::detail::apply_query_name_0x20(packet.data(), packet_len, random_bits), IoErr::None);

    std::array<char, 255> name_storage{};
    auto decoded = fiber::dns::decode_name(packet.data(), packet_len, 12, name_storage.data(), name_storage.size());
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->name, "AB-1.EXAMPLE");
    EXPECT_EQ(packet[decoded->next_offset], 0U);
    EXPECT_EQ(packet[decoded->next_offset + 1], static_cast<std::uint8_t>(RecordType::A));
}

TEST(DnsQuerySecurityTest, ResponseMatcherChecksIdAndCompleteQuestion) {
    std::array<std::uint8_t, 512> request{};
    std::array<std::uint8_t, 512> response{};
    const std::size_t request_len = encode_test_query(request, 0x1234U, "WwW.Example");
    const std::size_t response_len = encode_test_query(response, 0x1234U, "WwW.Example");
    write_be16(response.data() + 2, 0x8180U);

    EXPECT_TRUE(fiber::dns::detail::response_matches_query(request.data(), request_len, response.data(), response_len,
                                                           true));

    write_be16(response.data() + 2, 0x0180U);
    EXPECT_FALSE(fiber::dns::detail::response_matches_query(request.data(), request_len, response.data(), response_len,
                                                            true));
    write_be16(response.data() + 2, 0x8980U);
    EXPECT_FALSE(fiber::dns::detail::response_matches_query(request.data(), request_len, response.data(), response_len,
                                                            true));
    write_be16(response.data() + 2, 0x8180U);
    write_be16(response.data() + 4, 0);
    EXPECT_FALSE(fiber::dns::detail::response_matches_query(request.data(), request_len, response.data(), response_len,
                                                            true));
    write_be16(response.data() + 4, 1);

    write_be16(response.data(), 0x1235U);
    EXPECT_FALSE(fiber::dns::detail::response_matches_query(request.data(), request_len, response.data(), response_len,
                                                            true));
    write_be16(response.data(), 0x1234U);

    const std::size_t wrong_name_len = encode_test_query(response, 0x1234U, "wrong.example");
    write_be16(response.data() + 2, 0x8180U);
    EXPECT_FALSE(fiber::dns::detail::response_matches_query(request.data(), request_len, response.data(),
                                                            wrong_name_len, true));

    const std::size_t wrong_type_len =
            encode_test_query(response, 0x1234U, "WwW.Example", static_cast<std::uint16_t>(RecordType::AAAA));
    write_be16(response.data() + 2, 0x8180U);
    EXPECT_FALSE(fiber::dns::detail::response_matches_query(request.data(), request_len, response.data(),
                                                            wrong_type_len, true));

    const std::size_t wrong_class_len =
            encode_test_query(response, 0x1234U, "WwW.Example", static_cast<std::uint16_t>(RecordType::A), 2);
    write_be16(response.data() + 2, 0x8180U);
    EXPECT_FALSE(fiber::dns::detail::response_matches_query(request.data(), request_len, response.data(),
                                                            wrong_class_len, true));
}

TEST(DnsQuerySecurityTest, ResponseMatcherCanUseAsciiCaseInsensitiveNames) {
    std::array<std::uint8_t, 512> request{};
    std::array<std::uint8_t, 512> response{};
    const std::size_t request_len = encode_test_query(request, 7, "MiXeD.Example");
    const std::size_t response_len = encode_test_query(response, 7, "mixed.example");
    write_be16(response.data() + 2, 0x8180U);

    EXPECT_FALSE(fiber::dns::detail::response_matches_query(request.data(), request_len, response.data(), response_len,
                                                            true));
    EXPECT_TRUE(fiber::dns::detail::response_matches_query(request.data(), request_len, response.data(), response_len,
                                                           false));
}

} // namespace
