#include <gtest/gtest.h>

#include <cstdint>

#include "http/Http2HpackStaticTable.h"
#include "http/Http2HpackTableEntryView.h"
#include "http/HttpHeaderHash.h"

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

    const std::uint64_t name_hash = fiber::http::http_header_name_hash("content-type");
    ASSERT_TRUE(Http2HpackStaticTable::find_name("content-type", name_hash, index));
    EXPECT_EQ(index, 31u);

    EXPECT_FALSE(Http2HpackStaticTable::find_name("x-not-found", index));
}

TEST(Http2HpackStaticTableTest, FindExactMatchesKnownStaticEntries) {
    std::uint32_t index = 0;

    ASSERT_TRUE(Http2HpackStaticTable::find_exact(":method", "POST", index));
    EXPECT_EQ(index, 3u);

    const std::uint64_t name_hash = fiber::http::http_header_name_hash("accept-encoding");
    ASSERT_TRUE(Http2HpackStaticTable::find_exact("accept-encoding", name_hash, "gzip, deflate", index));
    EXPECT_EQ(index, 16u);

    EXPECT_FALSE(Http2HpackStaticTable::find_exact("accept-encoding", name_hash, "br", index));
}

} // namespace
