#include <gtest/gtest.h>

#include <cstdint>
#include <string_view>

#include <fiber/http_script/ConstPackage.h>

namespace {

using fiber::http_script::const_name_equal;
using fiber::http_script::const_name_hash;
using fiber::http_script::ConstPackage;
using fiber::http_script::ConstType;

TEST(ConstPackageTest, NormalizesAndDeduplicatesNamesWithinEachType) {
    ConstPackage::Builder builder;

    const auto *header = builder.add_constant(ConstType::Header, "X-Trace-ID");
    const auto *header_alias = builder.add_constant(ConstType::Header, "x_trace_id");
    const auto *query = builder.add_constant(ConstType::Query, "X-Trace-ID");

    ASSERT_NE(header, nullptr);
    EXPECT_EQ(header_alias, header);
    ASSERT_NE(query, nullptr);
    EXPECT_NE(query, header);

    auto package = builder.build();
    ASSERT_NE(package, nullptr);
    EXPECT_EQ(package->size(), 2U);
}

TEST(ConstPackageTest, BuilderCannotBeReusedAfterBuild) {
    ConstPackage::Builder builder;
    ASSERT_NE(builder.add_constant(ConstType::Header, "host"), nullptr);
    ASSERT_NE(builder.build(), nullptr);
    EXPECT_EQ(builder.add_constant(ConstType::Query, "late"), nullptr);
    EXPECT_EQ(builder.build(), nullptr);
}

TEST(ConstPackageTest, NameLengthIsCapped) {
    ConstPackage::Builder builder;
    const std::string max_name(fiber::http_script::kMaxConstNameLength, 'a');
    const std::string too_long(fiber::http_script::kMaxConstNameLength + 1, 'a');
    EXPECT_NE(builder.add_constant(ConstType::Header, max_name), nullptr);
    EXPECT_EQ(builder.add_constant(ConstType::Header, too_long), nullptr);
    auto package = builder.build();
    ASSERT_NE(package, nullptr);
    EXPECT_EQ(package->size(), 1U);
}

TEST(ConstPackageTest, ConstNameHashFoldsNormalization) {
    // The hash runs over normalized bytes, so case and '-'/'_' variants collide by design.
    EXPECT_EQ(const_name_hash("X-Trace-ID"), const_name_hash("x_trace_id"));
    EXPECT_EQ(const_name_hash(""), const_name_hash(""));
    EXPECT_NE(const_name_hash("a"), const_name_hash("b"));
}

TEST(ConstPackageTest, ConstNameEqualFoldsProbeNormalization) {
    // The package side stores the normalized key; arbitrary probes fold through the same
    // normalization before comparison.
    EXPECT_TRUE(const_name_equal("x_trace_id", "X-Trace-ID"));
    EXPECT_TRUE(const_name_equal("x_trace_id", "x-trace-id"));
    EXPECT_TRUE(const_name_equal("", ""));
    EXPECT_FALSE(const_name_equal("x_trace_id", "x_trace_ids"));
    EXPECT_FALSE(const_name_equal("abc", "abd"));
}

} // namespace
