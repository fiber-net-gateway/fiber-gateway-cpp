#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "http/Http2HpackDynamicTable.h"
#include "http/Http2HpackTableEntryView.h"
#include "http/HttpHeaderHash.h"

namespace {

using fiber::http::Http2HpackDynamicTable;

std::string repeated(char ch, std::size_t count) {
    return std::string(count, ch);
}

} // namespace

TEST(Http2HpackDynamicTableTest, InsertsAndGetsNewestFirstWithNameHash) {
    Http2HpackDynamicTable table;
    ASSERT_TRUE(table.init(4096));

    ASSERT_TRUE(table.insert("content-type", "text/plain"));
    ASSERT_TRUE(table.insert("accept", "*/*"));

    fiber::http::Http2HpackTableEntryView view;
    ASSERT_TRUE(table.get_by_index(1, view));
    EXPECT_EQ(view.name, "accept");
    EXPECT_EQ(view.value, "*/*");
    EXPECT_EQ(view.name_hash, fiber::http::http_header_name_hash("accept"));

    ASSERT_TRUE(table.get_by_index(2, view));
    EXPECT_EQ(view.name, "content-type");
    EXPECT_EQ(view.value, "text/plain");
    EXPECT_EQ(view.name_hash, fiber::http::http_header_name_hash("content-type"));
}

TEST(Http2HpackDynamicTableTest, FindExactReturnsNewestMatchingIndex) {
    Http2HpackDynamicTable table;
    ASSERT_TRUE(table.init(4096));

    ASSERT_TRUE(table.insert("x-key", "v1"));
    ASSERT_TRUE(table.insert("x-other", "v2"));
    ASSERT_TRUE(table.insert("x-key", "v1"));

    std::uint32_t dynamic_index = 0;
    ASSERT_TRUE(table.find_exact("X-Key", "v1", dynamic_index));
    EXPECT_EQ(dynamic_index, 1u);
}

TEST(Http2HpackDynamicTableTest, EvictsOldestEntryWhenCapacityWouldBeExceeded) {
    Http2HpackDynamicTable table;
    ASSERT_TRUE(table.init(70));

    ASSERT_TRUE(table.insert("a", "1"));
    ASSERT_TRUE(table.insert("b", "2"));
    ASSERT_TRUE(table.insert("c", "3"));

    EXPECT_EQ(table.entry_count(), 2u);

    std::uint32_t dynamic_index = 0;
    EXPECT_FALSE(table.find_exact("a", "1", dynamic_index));
    ASSERT_TRUE(table.find_exact("b", "2", dynamic_index));
    EXPECT_EQ(dynamic_index, 2u);
    ASSERT_TRUE(table.find_exact("c", "3", dynamic_index));
    EXPECT_EQ(dynamic_index, 1u);
}

TEST(Http2HpackDynamicTableTest, CompactsBytesWhenTailSpaceIsInsufficient) {
    Http2HpackDynamicTable table;
    ASSERT_TRUE(table.init(205));

    const std::string name_a = repeated('a', 10);
    const std::string name_b = repeated('b', 10);
    const std::string name_c = repeated('c', 10);
    const std::string value_a = repeated('x', 60);
    const std::string value_b = repeated('y', 60);
    const std::string value_c = repeated('z', 60);

    ASSERT_TRUE(table.insert(name_a, value_a));
    ASSERT_TRUE(table.insert(name_b, value_b));
    ASSERT_TRUE(table.insert(name_c, value_c));

    EXPECT_EQ(table.entry_count(), 2u);

    fiber::http::Http2HpackTableEntryView view;
    ASSERT_TRUE(table.get_by_index(1, view));
    EXPECT_EQ(view.name, name_c);
    EXPECT_EQ(view.value, value_c);

    ASSERT_TRUE(table.get_by_index(2, view));
    EXPECT_EQ(view.name, name_b);
    EXPECT_EQ(view.value, value_b);
}

TEST(Http2HpackDynamicTableTest, SetMaxSizeShrinksAndOversizedInsertClearsTable) {
    Http2HpackDynamicTable table;
    ASSERT_TRUE(table.init(128));

    ASSERT_TRUE(table.insert("alpha", "1"));
    ASSERT_TRUE(table.insert("beta", "2"));
    table.set_max_size(40);

    EXPECT_LE(table.current_size(), 40u);
    EXPECT_EQ(table.entry_count(), 1u);

    const std::string large_name = repeated('n', 16);
    const std::string large_value = repeated('v', 40);
    EXPECT_FALSE(table.insert(large_name, large_value));
    EXPECT_TRUE(table.empty());
}
