#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "http/Http2HpackEncodeCatalog.h"
#include "http/Http2HpackStaticTable.h"
#include "http/HttpHeaderHash.h"

namespace {

using fiber::http::Http2HpackEncodeCatalog;

TEST(Http2HpackEncodeCatalogTest, FindReturnsStaticExactAndStaticNameMatches) {
    Http2HpackEncodeCatalog catalog;
    ASSERT_TRUE(catalog.init({}));

    const auto exact = catalog.find(":status", fiber::http::http_header_name_hash(":status"), "200");
    ASSERT_NE(exact.entry, nullptr);
    EXPECT_TRUE(exact.exact);
    EXPECT_EQ(exact.entry->kind, Http2HpackEncodeCatalog::EntryKind::Static);
    EXPECT_EQ(exact.entry->hpack_index, 8u);

    const auto name_only =
            catalog.find("content-type", fiber::http::http_header_name_hash("content-type"), "text/plain");
    ASSERT_NE(name_only.entry, nullptr);
    EXPECT_FALSE(name_only.exact);
    EXPECT_EQ(name_only.entry->kind, Http2HpackEncodeCatalog::EntryKind::Static);
    EXPECT_EQ(name_only.entry->hpack_index, 31u);
}

TEST(Http2HpackEncodeCatalogTest, FindReturnsPolicyEntriesWhenStaticTableMisses) {
    constexpr std::array<Http2HpackEncodeCatalog::PolicyEntry, 2> kPolicies{{
            {"server", fiber::http::http_header_name_hash("server"), "nginx-1.25.1"},
            {"x-powered-by", fiber::http::http_header_name_hash("x-powered-by"), "openresty"},
    }};

    Http2HpackEncodeCatalog catalog;
    ASSERT_TRUE(catalog.init(kPolicies));

    const auto exact = catalog.find("x-powered-by", fiber::http::http_header_name_hash("x-powered-by"), "openresty");
    ASSERT_NE(exact.entry, nullptr);
    EXPECT_TRUE(exact.exact);
    EXPECT_TRUE(catalog.is_policy_entry(exact.entry));
    EXPECT_EQ(exact.entry->kind, Http2HpackEncodeCatalog::EntryKind::Policy);
    EXPECT_EQ(exact.entry->value, "openresty");

    const auto name_only = catalog.find("x-powered-by", fiber::http::http_header_name_hash("x-powered-by"), "unit");
    ASSERT_NE(name_only.entry, nullptr);
    EXPECT_FALSE(name_only.exact);
    EXPECT_TRUE(catalog.is_policy_entry(name_only.entry));
}

} // namespace
