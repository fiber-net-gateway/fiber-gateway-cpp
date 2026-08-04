#include <gtest/gtest.h>

#include <iterator>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "http/HeaderMap.h"
#include "http/HttpHeaderHash.h"

namespace {

using IntHeaderMap = fiber::http::HeaderMap<int>;

static_assert(noexcept(IntHeaderMap{}));
static_assert(std::is_nothrow_move_constructible_v<IntHeaderMap>);
static_assert(std::is_nothrow_move_assignable_v<IntHeaderMap>);
static_assert(std::forward_iterator<IntHeaderMap::ConstIterator>);
static_assert(noexcept(std::declval<const IntHeaderMap &>().get({})));
static_assert(noexcept(std::declval<const IntHeaderMap &>().contains({})));
static_assert(noexcept(std::declval<const IntHeaderMap &>().size()));
static_assert(noexcept(std::declval<const IntHeaderMap &>().empty()));
static_assert(noexcept(std::declval<const IntHeaderMap &>().begin()));
static_assert(noexcept(std::declval<const IntHeaderMap &>().end()));
static_assert(noexcept(++std::declval<IntHeaderMap::ConstIterator &>()));

IntHeaderMap build_int_map() {
    IntHeaderMap::Builder builder(3);
    EXPECT_TRUE(builder.insert("content-length", 42));
    EXPECT_TRUE(builder.insert("connection", 7));
    EXPECT_TRUE(builder.insert("expect", 9));
    return std::move(builder).build();
}

} // namespace

TEST(HeaderMapTest, MatchesCaseInsensitiveLookupsAndCachedHash) {
    IntHeaderMap map = build_int_map();

    const auto cached_hash = fiber::http::http_header_name_hash("Content-Length");

    const int *mixed_case = map.get("Content-Length");
    ASSERT_NE(mixed_case, nullptr);
    EXPECT_EQ(*mixed_case, 42);

    const int *cached = map.get("content-length", cached_hash);
    ASSERT_NE(cached, nullptr);
    EXPECT_EQ(*cached, 42);
    EXPECT_TRUE(map.contains("CONTENT-LENGTH"));
    EXPECT_TRUE(map.contains("content-length", cached_hash));
    EXPECT_FALSE(map.contains("missing"));
}

TEST(HeaderMapTest, RejectsCaseInsensitiveDuplicateKeysInBuilder) {
    IntHeaderMap::Builder builder;
    ASSERT_TRUE(builder.insert("connection", fiber::http::http_header_name_hash("connection"), 1));
    EXPECT_FALSE(builder.insert("Connection", fiber::http::http_header_name_hash("Connection"), 2));
    EXPECT_EQ(builder.size(), 1U);

    IntHeaderMap map = std::move(builder).build();
    ASSERT_NE(map.get("CONNECTION"), nullptr);
    EXPECT_EQ(*map.get("CONNECTION"), 1);
}

TEST(HeaderMapTest, IteratesInInsertionOrderWithOriginalNamesAndHashes) {
    IntHeaderMap map = build_int_map();

    std::vector<std::string> names;
    std::vector<int> values;
    std::vector<std::uint64_t> hashes;
    for (const auto entry: map) {
        names.emplace_back(entry.name());
        values.push_back(entry.value());
        hashes.push_back(entry.hash());
    }

    EXPECT_EQ(names, (std::vector<std::string>{"content-length", "connection", "expect"}));
    EXPECT_EQ(values, (std::vector<int>{42, 7, 9}));
    EXPECT_EQ(hashes, (std::vector<std::uint64_t>{
                              fiber::http::http_header_name_hash("content-length"),
                              fiber::http::http_header_name_hash("connection"),
                              fiber::http::http_header_name_hash("expect"),
                      }));
}

TEST(HeaderMapTest, HandlesOpenAddressingCollisions) {
    IntHeaderMap::Builder builder(3);
    // These one-byte names occupy the same initial slot in a four-bucket table.
    ASSERT_TRUE(builder.insert("a", 1));
    ASSERT_TRUE(builder.insert("e", 2));
    ASSERT_TRUE(builder.insert("i", 3));
    IntHeaderMap map = std::move(builder).build();

    ASSERT_NE(map.get("A"), nullptr);
    ASSERT_NE(map.get("E"), nullptr);
    ASSERT_NE(map.get("I"), nullptr);
    EXPECT_EQ(*map.get("A"), 1);
    EXPECT_EQ(*map.get("E"), 2);
    EXPECT_EQ(*map.get("I"), 3);
    EXPECT_EQ(map.get("m"), nullptr);
}

TEST(HeaderMapTest, EmptyMapDoesNotExposeEntries) {
    IntHeaderMap::Builder builder(8);
    IntHeaderMap map = std::move(builder).build();

    EXPECT_TRUE(map.empty());
    EXPECT_EQ(map.size(), 0U);
    EXPECT_EQ(map.begin(), map.end());
    EXPECT_EQ(map.get("anything"), nullptr);
}

TEST(HeaderMapTest, CopiesPackedNamesAndNonTrivialValues) {
    fiber::http::HeaderMap<std::string>::Builder builder(2);
    ASSERT_TRUE(builder.insert("X-First-Long-Header", std::string("first")));
    ASSERT_TRUE(builder.insert("X-Second-Long-Header", std::string("second")));
    auto original = std::move(builder).build();

    auto copy = original;
    original = {};

    ASSERT_NE(copy.get("x-first-long-header"), nullptr);
    ASSERT_NE(copy.get("X-SECOND-LONG-HEADER"), nullptr);
    EXPECT_EQ(*copy.get("x-first-long-header"), "first");
    EXPECT_EQ(*copy.get("X-SECOND-LONG-HEADER"), "second");
}

TEST(HeaderMapTest, RehashesDenseBuilderInput) {
    IntHeaderMap::Builder builder(64);
    for (int i = 0; i < 64; ++i) {
        ASSERT_TRUE(builder.insert("x-header-" + std::to_string(i), i));
    }
    IntHeaderMap map = std::move(builder).build();

    EXPECT_EQ(map.size(), 64U);
    for (int i = 0; i < 64; ++i) {
        const int *value = map.get("X-HEADER-" + std::to_string(i));
        ASSERT_NE(value, nullptr);
        EXPECT_EQ(*value, i);
    }
}
