#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "http/Http2HpackDecodeTable.h"
#include "http/Http2HpackTableEntryView.h"
#include "http/HttpHeaderHash.h"

namespace {

using fiber::http::Http2HpackDecodeTable;

std::string repeated(char ch, std::size_t count) {
    return std::string(count, ch);
}

} // namespace

TEST(Http2HpackDecodeTableTest, InsertsAndGetsNewestFirstWithNameHash) {
    Http2HpackDecodeTable table;
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

TEST(Http2HpackDecodeTableTest, EvictsOldestEntryWhenCapacityWouldBeExceeded) {
    Http2HpackDecodeTable table;
    ASSERT_TRUE(table.init(70));

    ASSERT_TRUE(table.insert("a", "1"));
    ASSERT_TRUE(table.insert("b", "2"));
    ASSERT_TRUE(table.insert("c", "3"));

    EXPECT_EQ(table.entry_count(), 2u);

    fiber::http::Http2HpackTableEntryView view;
    ASSERT_TRUE(table.get_by_index(1, view));
    EXPECT_EQ(view.name, "c");
    EXPECT_EQ(view.value, "3");
    ASSERT_TRUE(table.get_by_index(2, view));
    EXPECT_EQ(view.name, "b");
    EXPECT_EQ(view.value, "2");
    EXPECT_FALSE(table.get_by_index(3, view));
}

TEST(Http2HpackDecodeTableTest, CompactsBytesWhenTailSpaceIsInsufficient) {
    Http2HpackDecodeTable table;
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

TEST(Http2HpackDecodeTableTest, SetMaxSizeShrinksAndOversizedInsertClearsTable) {
    Http2HpackDecodeTable table;
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
