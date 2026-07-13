#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "http/Http2HpackEncodeCatalog.h"
#include "http/HttpHeaderHash.h"

namespace {

using fiber::http::Http2HpackEncodeCatalog;

TEST(Http2HpackEncodeCatalogTest, FindReturnsStaticExactAndStaticNameMatches) {
    Http2HpackEncodeCatalog catalog;
    ASSERT_TRUE(catalog.init({}));

    const auto exact = catalog.find(":status", fiber::http::http_header_name_hash(":status"), "200");
    ASSERT_NE(exact.exact_entry, nullptr);
    ASSERT_NE(exact.name_entry, nullptr);
    EXPECT_EQ(exact.exact_entry->kind, Http2HpackEncodeCatalog::EntryKind::Static);
    EXPECT_EQ(exact.exact_entry->hpack_index, 8u);
    EXPECT_EQ(exact.name_entry->hpack_index, 8u);

    const auto name_only =
            catalog.find("content-type", fiber::http::http_header_name_hash("content-type"), "text/plain");
    EXPECT_EQ(name_only.exact_entry, nullptr);
    ASSERT_NE(name_only.name_entry, nullptr);
    EXPECT_EQ(name_only.name_entry->kind, Http2HpackEncodeCatalog::EntryKind::Static);
    EXPECT_EQ(name_only.name_entry->hpack_index, 31u);

    const auto mixed_case =
            catalog.find("Content-Type", fiber::http::http_header_name_hash("Content-Type"), "text/plain");
    EXPECT_EQ(mixed_case.exact_entry, nullptr);
    ASSERT_NE(mixed_case.name_entry, nullptr);
    EXPECT_EQ(mixed_case.name_entry->hpack_index, 31u);
}

TEST(Http2HpackEncodeCatalogTest, FindReturnsPolicyEntriesWhenStaticTableMisses) {
    constexpr std::array<Http2HpackEncodeCatalog::PolicyEntry, 2> kPolicies{{
            {"server", fiber::http::http_header_name_hash("server"), "nginx-1.25.1"},
            {"x-powered-by", fiber::http::http_header_name_hash("x-powered-by"), "openresty"},
    }};

    Http2HpackEncodeCatalog catalog;
    ASSERT_TRUE(catalog.init(kPolicies));

    const auto exact = catalog.find("x-powered-by", fiber::http::http_header_name_hash("x-powered-by"), "openresty");
    ASSERT_NE(exact.exact_entry, nullptr);
    ASSERT_NE(exact.name_entry, nullptr);
    EXPECT_TRUE(catalog.is_policy_entry(exact.exact_entry));
    EXPECT_TRUE(catalog.is_policy_entry(exact.name_entry));
    EXPECT_EQ(exact.exact_entry->value, "openresty");

    const auto name_only = catalog.find("x-powered-by", fiber::http::http_header_name_hash("x-powered-by"), "unit");
    EXPECT_EQ(name_only.exact_entry, nullptr);
    ASSERT_NE(name_only.name_entry, nullptr);
    EXPECT_TRUE(catalog.is_policy_entry(name_only.name_entry));
}

TEST(Http2HpackEncodeCatalogTest, FindKeepsStaticNameForExactPolicyValue) {
    constexpr std::array<Http2HpackEncodeCatalog::PolicyEntry, 1> kPolicies{{
            {"server", fiber::http::http_header_name_hash("server"), "nginx-1.25.1"},
    }};

    Http2HpackEncodeCatalog catalog;
    ASSERT_TRUE(catalog.init(kPolicies));

    const auto static_name = catalog.find("server", fiber::http::http_header_name_hash("server"), "nginx-1.25.1");
    ASSERT_NE(static_name.exact_entry, nullptr);
    ASSERT_NE(static_name.name_entry, nullptr);
    EXPECT_TRUE(catalog.is_policy_entry(static_name.exact_entry));
    EXPECT_TRUE(catalog.is_static_entry(static_name.name_entry));
    EXPECT_EQ(static_name.name_entry->hpack_index, 54u);
}

} // namespace
