#include <gtest/gtest.h>

#include <cstdint>

#include <fiber/http/Http2HpackStaticTable.h>
#include <fiber/http/Http2HpackTableEntryView.h>
#include <fiber/http/HttpHeaderHash.h>

namespace {

using fiber::http::Http2HpackStaticTable;

TEST(Http2HpackStaticTableTest, GetByIndexReturnsKnownEntriesAndNameHash) {
    fiber::http::Http2HpackTableEntryView view;

    ASSERT_TRUE(Http2HpackStaticTable::get_by_index(2, view));
    EXPECT_EQ(view.name, ":method");
    EXPECT_EQ(view.value, "GET");
    EXPECT_EQ(view.name_hash, fiber::http::http_header_name_hash(":method"));

    ASSERT_TRUE(Http2HpackStaticTable::get_by_index(61, view));
    EXPECT_EQ(view.name, "www-authenticate");
    EXPECT_EQ(view.value, "");
    EXPECT_EQ(view.name_hash, fiber::http::http_header_name_hash("www-authenticate"));

    EXPECT_FALSE(Http2HpackStaticTable::get_by_index(0, view));
    EXPECT_FALSE(Http2HpackStaticTable::get_by_index(62, view));
}

TEST(Http2HpackStaticTableTest, FindNameReturnsSmallestMatchingIndex) {
    std::uint32_t index = 0;

    ASSERT_TRUE(Http2HpackStaticTable::find_name(":Method", index));
    EXPECT_EQ(index, 2u);

    const std::uint64_t name_hash = fiber::http::http_header_name_hash("Content-Type");
    ASSERT_TRUE(Http2HpackStaticTable::find_name("Content-Type", name_hash, index));
    EXPECT_EQ(index, 31u);

    EXPECT_FALSE(Http2HpackStaticTable::find_name("x-not-found", index));
}

TEST(Http2HpackStaticTableTest, FindReturnsExactAndNameMatches) {
    const auto exact = Http2HpackStaticTable::find(":status", fiber::http::http_header_name_hash(":status"), "200");
    EXPECT_EQ(exact.exact_index, 8u);
    EXPECT_EQ(exact.name_index, 8u);

    const auto name_only = Http2HpackStaticTable::find(
            "content-type", fiber::http::http_header_name_hash("content-type"), "text/plain");
    EXPECT_EQ(name_only.exact_index, 0u);
    EXPECT_EQ(name_only.name_index, 31u);

    const auto mixed_case = Http2HpackStaticTable::find(
            "Content-Type", fiber::http::http_header_name_hash("Content-Type"), "text/plain");
    EXPECT_EQ(mixed_case.exact_index, 0u);
    EXPECT_EQ(mixed_case.name_index, 31u);

    const auto missing = Http2HpackStaticTable::find("x-powered-by", fiber::http::http_header_name_hash("x-powered-by"),
                                                     "openresty");
    EXPECT_EQ(missing.exact_index, 0u);
    EXPECT_EQ(missing.name_index, 0u);
}

TEST(Http2HpackStaticTableTest, FindResolvesEveryStaticEntry) {
    for (std::uint32_t index = 1; index <= Http2HpackStaticTable::kEntryCount; ++index) {
        fiber::http::Http2HpackTableEntryView view;
        ASSERT_TRUE(Http2HpackStaticTable::get_by_index(index, view));

        const auto result = Http2HpackStaticTable::find(view.name, view.name_hash, view.value);
        EXPECT_EQ(result.exact_index, index) << view.name << ": " << view.value;

        std::uint32_t first_name_index = 0;
        ASSERT_TRUE(Http2HpackStaticTable::find_name(view.name, view.name_hash, first_name_index));
        EXPECT_EQ(result.name_index, first_name_index) << view.name;
    }
}

} // namespace
