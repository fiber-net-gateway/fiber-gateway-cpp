#include <gtest/gtest.h>

#include <fiber/http/HttpConnectionGroupHintTable.h>

namespace {

using fiber::http::HttpConnectionGroupHintTable;
using fiber::http::HttpConnectionGroupKey;
using fiber::http::HttpConnectionPoolAffinity;

TEST(HttpConnectionGroupHintTableTest, TracksApproximateCountPerGroupKey) {
    HttpConnectionGroupHintTable table;
    auto key = HttpConnectionGroupKey::from_name("Example.COM", 443, HttpConnectionGroupKey::Scheme::Https);
    ASSERT_TRUE(key.has_value());

    EXPECT_EQ(table.probe(*key).approx_count, 0);

    table.note_idle_add(*key);
    EXPECT_EQ(table.probe(*key).approx_count, 1);

    table.note_idle_add(*key);
    EXPECT_EQ(table.probe(*key).approx_count, 2);

    table.note_idle_remove(*key);
    EXPECT_EQ(table.probe(*key).approx_count, 1);

    table.note_idle_remove(*key);
    EXPECT_EQ(table.probe(*key).approx_count, 0);

    table.note_idle_remove(*key);
    EXPECT_EQ(table.probe(*key).approx_count, 0);
}

TEST(HttpConnectionGroupHintTableTest, SeparatesDifferentGroups) {
    HttpConnectionGroupHintTable table;
    auto http_key = HttpConnectionGroupKey::from_name("example.com", 80, HttpConnectionGroupKey::Scheme::Http);
    auto https_key = HttpConnectionGroupKey::from_name("example.com", 443, HttpConnectionGroupKey::Scheme::Https);
    ASSERT_TRUE(http_key.has_value());
    ASSERT_TRUE(https_key.has_value());

    table.note_idle_add(*http_key);

    EXPECT_EQ(table.probe(*http_key).approx_count, 1);
    EXPECT_EQ(table.probe(*https_key).approx_count, 0);
}

TEST(HttpConnectionGroupHintTableTest, ClearRemovesPublishedHints) {
    HttpConnectionGroupHintTable table;
    const auto ip_key = HttpConnectionGroupKey::from_ip(fiber::net::IpAddress::v4({127, 0, 0, 1}), 8080,
                                                        HttpConnectionGroupKey::Scheme::Http);

    table.note_idle_add(ip_key);
    ASSERT_EQ(table.probe(ip_key).approx_count, 1);

    table.clear();
    EXPECT_EQ(table.probe(ip_key).approx_count, 0);
}

TEST(HttpConnectionGroupHintTableTest, CountSaturatesAtMaxApproxCount) {
    HttpConnectionGroupHintTable table;
    auto key = HttpConnectionGroupKey::from_name("example.com", 8080, HttpConnectionGroupKey::Scheme::Http);
    ASSERT_TRUE(key.has_value());

    for (std::size_t i = 0; i < static_cast<std::size_t>(HttpConnectionGroupHintTable::kMaxApproxCount) + 32; ++i) {
        table.note_idle_add(*key);
    }

    EXPECT_EQ(table.probe(*key).approx_count, HttpConnectionGroupHintTable::kMaxApproxCount);
}

TEST(HttpConnectionGroupHintTableTest, SeparatesPoolAffinitiesForTheSameEndpoint) {
    HttpConnectionGroupHintTable table;
    auto first = HttpConnectionGroupKey::from_name("example.com", 443, HttpConnectionGroupKey::Scheme::Https,
                                                   HttpConnectionPoolAffinity{11});
    auto second = HttpConnectionGroupKey::from_name("example.com", 443, HttpConnectionGroupKey::Scheme::Https,
                                                    HttpConnectionPoolAffinity{12});
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());

    table.note_idle_add(*first);

    EXPECT_EQ(table.probe(*first).approx_count, 1);
    EXPECT_EQ(table.probe(*second).approx_count, 0);
}

} // namespace
