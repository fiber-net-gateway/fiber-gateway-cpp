#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "http/Http2HpackEncodeCatalog.h"
#include "http/Http2HpackEncodeTable.h"
#include "http/Http2HpackStaticTable.h"
#include "http/HttpHeaderHash.h"

namespace {

using fiber::http::Http2HpackEncodeCatalog;
using fiber::http::Http2HpackEncodeTable;
using fiber::http::Http2HpackStaticTable;

TEST(Http2HpackEncodeTableTest, ResolveIndexReturnsStaticAndDynamicIndexes) {
    constexpr std::array<Http2HpackEncodeCatalog::PolicyEntry, 2> kPolicies{{
            {"server", fiber::http::http_header_name_hash("server"), "nginx-1.25.1"},
            {"x-powered-by", fiber::http::http_header_name_hash("x-powered-by"), "openresty"},
    }};

    Http2HpackEncodeCatalog catalog;
    ASSERT_TRUE(catalog.init(kPolicies));

    Http2HpackEncodeTable table;
    ASSERT_TRUE(table.init(catalog, 256));

    const auto static_result = catalog.find(":status", fiber::http::http_header_name_hash(":status"), "200");
    ASSERT_NE(static_result.exact_entry, nullptr);
    std::uint32_t static_index = 0;
    ASSERT_TRUE(table.resolve_index(static_result.exact_entry, static_index));
    EXPECT_EQ(static_index, 8u);

    const auto server_result = catalog.find("server", fiber::http::http_header_name_hash("server"), "nginx-1.25.1");
    const auto powered_by_result =
            catalog.find("x-powered-by", fiber::http::http_header_name_hash("x-powered-by"), "openresty");
    ASSERT_NE(server_result.exact_entry, nullptr);
    ASSERT_NE(powered_by_result.exact_entry, nullptr);

    EXPECT_EQ(table.activate(server_result.exact_entry), Http2HpackEncodeTable::ActivateResult::Activated);
    EXPECT_EQ(table.activate(powered_by_result.exact_entry), Http2HpackEncodeTable::ActivateResult::Activated);

    std::uint32_t server_index = 0;
    std::uint32_t powered_by_index = 0;
    ASSERT_TRUE(table.resolve_index(server_result.exact_entry, server_index));
    ASSERT_TRUE(table.resolve_index(powered_by_result.exact_entry, powered_by_index));

    EXPECT_EQ(powered_by_index, Http2HpackStaticTable::kEntryCount + 1);
    EXPECT_EQ(server_index, Http2HpackStaticTable::kEntryCount + 2);
}

TEST(Http2HpackEncodeTableTest, ShrinkEvictsOldestPoliciesAndTracksPendingSizeUpdate) {
    constexpr std::array<Http2HpackEncodeCatalog::PolicyEntry, 2> kPolicies{{
            {"server", fiber::http::http_header_name_hash("server"), "nginx-1.25.1"},
            {"x-powered-by", fiber::http::http_header_name_hash("x-powered-by"), "openresty"},
    }};

    Http2HpackEncodeCatalog catalog;
    ASSERT_TRUE(catalog.init(kPolicies));

    const auto server_result = catalog.find("server", fiber::http::http_header_name_hash("server"), "nginx-1.25.1");
    const auto powered_by_result =
            catalog.find("x-powered-by", fiber::http::http_header_name_hash("x-powered-by"), "openresty");
    ASSERT_NE(server_result.exact_entry, nullptr);
    ASSERT_NE(powered_by_result.exact_entry, nullptr);

    Http2HpackEncodeTable table;
    ASSERT_TRUE(table.init(catalog, server_result.exact_entry->entry_size + powered_by_result.exact_entry->entry_size));

    EXPECT_EQ(table.activate(server_result.exact_entry), Http2HpackEncodeTable::ActivateResult::Activated);
    EXPECT_EQ(table.activate(powered_by_result.exact_entry), Http2HpackEncodeTable::ActivateResult::Activated);
    EXPECT_EQ(table.active_count(), 2u);

    table.update_max_dynamic_table_size(powered_by_result.exact_entry->entry_size);
    EXPECT_TRUE(table.has_pending_table_size_update());
    EXPECT_EQ(table.pending_dynamic_table_size(), powered_by_result.exact_entry->entry_size);
    EXPECT_EQ(table.active_count(), 1u);
    EXPECT_FALSE(table.is_active(server_result.exact_entry));
    EXPECT_TRUE(table.is_active(powered_by_result.exact_entry));

    std::uint32_t index = 0;
    EXPECT_FALSE(table.resolve_index(server_result.exact_entry, index));
    ASSERT_TRUE(table.resolve_index(powered_by_result.exact_entry, index));
    EXPECT_EQ(index, Http2HpackStaticTable::kEntryCount + 1);

    table.acknowledge_table_size_update();
    EXPECT_FALSE(table.has_pending_table_size_update());
}

} // namespace
