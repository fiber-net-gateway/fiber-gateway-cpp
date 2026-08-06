#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "http_script/ConstPackage.h"

namespace {

using fiber::http_script::ConstPackage;
using fiber::http_script::ConstType;
using fiber::http_script::kInvalidConstIndex;

std::uint64_t normalized_hash(std::string_view name) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char ch: name) {
        if (ch == '-') {
            ch = '_';
        } else if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<unsigned char>(ch - 'A' + 'a');
        }
        hash ^= ch;
        hash *= 1099511628211ULL;
    }
    return hash;
}

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
    EXPECT_EQ(package->find(ConstType::Header, "x-trace-id"), package->find(ConstType::Header, "X_TRACE_ID"));
    EXPECT_NE(package->find(ConstType::Header, "x_trace_id"), package->find(ConstType::Query, "x_trace_id"));
    EXPECT_EQ(package->find(ConstType::Cookie, "x_trace_id"), kInvalidConstIndex);
}

TEST(ConstPackageTest, BuildsTypeContiguousImmutableIndices) {
    ConstPackage::Builder builder;
    ASSERT_NE(builder.add_constant(ConstType::Context, "cluster"), nullptr);
    ASSERT_NE(builder.add_constant(ConstType::Path, "id"), nullptr);
    ASSERT_NE(builder.add_constant(ConstType::Header, "host"), nullptr);
    ASSERT_NE(builder.add_constant(ConstType::Path, "tail"), nullptr);

    auto package = builder.build();
    ASSERT_NE(package, nullptr);
    ASSERT_EQ(package->entries(ConstType::Path).size(), 2U);
    ASSERT_EQ(package->entries(ConstType::Header).size(), 1U);
    ASSERT_EQ(package->entries(ConstType::Context).size(), 1U);
    EXPECT_EQ(package->entries(ConstType::Path)[0].index, 0U);
    EXPECT_EQ(package->entries(ConstType::Path)[1].index, 1U);
    EXPECT_EQ(package->entries(ConstType::Header)[0].index, 2U);
    EXPECT_EQ(package->entries(ConstType::Context)[0].index, 3U);

    EXPECT_EQ(builder.add_constant(ConstType::Query, "late"), nullptr);
    EXPECT_EQ(builder.build(), nullptr);
}

TEST(ConstPackageTest, QuadraticProbeFindsAFullCollisionChain) {
    ConstPackage::Builder builder;
    std::vector<std::string> names;
    for (std::uint32_t value = 0; names.size() < 8; ++value) {
        std::string name = "collision_" + std::to_string(value);
        if ((normalized_hash(name) & 15U) == 0) {
            names.push_back(std::move(name));
        }
    }
    for (const std::string &name: names) {
        ASSERT_NE(builder.add_constant(ConstType::Header, name), nullptr);
    }

    auto package = builder.build();
    ASSERT_NE(package, nullptr);
    ASSERT_EQ(package->entries(ConstType::Header).size(), names.size());
    for (std::size_t i = 0; i < names.size(); ++i) {
        EXPECT_EQ(package->find(ConstType::Header, names[i]), i);
    }
}

} // namespace
