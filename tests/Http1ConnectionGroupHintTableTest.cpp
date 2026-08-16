#include <gtest/gtest.h>

#include <fiber/http/Http1ConnectionGroupHintTable.h>

namespace {

using fiber::http::Http1ConnectionGroupHintTable;
using fiber::http::Http1ConnectionGroupKey;
using fiber::http::Http1ConnectionPoolAffinity;

TEST(Http1ConnectionGroupHintTableTest, TracksApproximateCountPerGroupKey) {
    Http1ConnectionGroupHintTable table;
    auto key = Http1ConnectionGroupKey::from_name("Example.COM", 443, Http1ConnectionGroupKey::Scheme::Https);
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

TEST(Http1ConnectionGroupHintTableTest, SeparatesDifferentGroups) {
    Http1ConnectionGroupHintTable table;
    auto http_key = Http1ConnectionGroupKey::from_name("example.com", 80, Http1ConnectionGroupKey::Scheme::Http);
    auto https_key = Http1ConnectionGroupKey::from_name("example.com", 443, Http1ConnectionGroupKey::Scheme::Https);
    ASSERT_TRUE(http_key.has_value());
    ASSERT_TRUE(https_key.has_value());

    table.note_idle_add(*http_key);

    EXPECT_EQ(table.probe(*http_key).approx_count, 1);
    EXPECT_EQ(table.probe(*https_key).approx_count, 0);
}

TEST(Http1ConnectionGroupHintTableTest, ClearRemovesPublishedHints) {
    Http1ConnectionGroupHintTable table;
    const auto ip_key = Http1ConnectionGroupKey::from_ip(fiber::net::IpAddress::v4({127, 0, 0, 1}), 8080,
                                                         Http1ConnectionGroupKey::Scheme::Http);

    table.note_idle_add(ip_key);
    ASSERT_EQ(table.probe(ip_key).approx_count, 1);

    table.clear();
    EXPECT_EQ(table.probe(ip_key).approx_count, 0);
}

TEST(Http1ConnectionGroupHintTableTest, CountSaturatesAtMaxApproxCount) {
    Http1ConnectionGroupHintTable table;
    auto key = Http1ConnectionGroupKey::from_name("example.com", 8080, Http1ConnectionGroupKey::Scheme::Http);
    ASSERT_TRUE(key.has_value());

    for (std::size_t i = 0; i < static_cast<std::size_t>(Http1ConnectionGroupHintTable::kMaxApproxCount) + 32; ++i) {
        table.note_idle_add(*key);
    }

    EXPECT_EQ(table.probe(*key).approx_count, Http1ConnectionGroupHintTable::kMaxApproxCount);
}

TEST(Http1ConnectionGroupHintTableTest, SeparatesPoolAffinitiesForTheSameEndpoint) {
    Http1ConnectionGroupHintTable table;
    auto first = Http1ConnectionGroupKey::from_name("example.com", 443, Http1ConnectionGroupKey::Scheme::Https,
                                                    Http1ConnectionPoolAffinity{11});
    auto second = Http1ConnectionGroupKey::from_name("example.com", 443, Http1ConnectionGroupKey::Scheme::Https,
                                                     Http1ConnectionPoolAffinity{12});
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());

    table.note_idle_add(*first);

    EXPECT_EQ(table.probe(*first).approx_count, 1);
    EXPECT_EQ(table.probe(*second).approx_count, 0);
}

} // namespace
