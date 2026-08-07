#include <gtest/gtest.h>

#include <fiber/common/mem/BufPool.h>

#include "observability/AccessTraceState.h"

namespace {

TEST(AccessTraceStateTest, DecodesJavaGmpBase62AndRebuildsTraceState) {
    fiber::mem::BufPool pool;
    fiber::access_server::AccessTraceState state(pool);

    state.parse("vendor=opaque,bnrc=2F0tFt-1tSBej");

    ASSERT_TRUE(state.should_override_upstream());
    ASSERT_EQ(state.get_context("zone"), std::optional<std::string_view>("gray"));
    auto encoded = state.encode();
    ASSERT_TRUE(encoded);
    EXPECT_EQ(*encoded, "vendor=opaque,bnrc=2F0tFt-1tSBej");
}

TEST(AccessTraceStateTest, AppliesContextUpdatesBeforeEncoding) {
    fiber::mem::BufPool pool;
    fiber::access_server::AccessTraceState state(pool);
    state.parse("vendor=opaque,bnrc=2F0tFt-1tSBej");

    ASSERT_TRUE(state.put_context("tenant", "blue"));
    ASSERT_TRUE(state.remove_context("zone"));

    auto encoded = state.encode();
    ASSERT_TRUE(encoded);
    EXPECT_EQ(*encoded, "vendor=opaque,bnrc=aL8nZlUy-1nka5V");
}

TEST(AccessTraceStateTest, TreatsUtf8AsJavaCompatibleBytes) {
    fiber::mem::BufPool pool;
    fiber::access_server::AccessTraceState state(pool);

    state.parse("bnrc=4PCog3-12EcS");

    ASSERT_EQ(state.get_context("🥧"), std::optional<std::string_view>("陈"));
    auto encoded = state.encode();
    ASSERT_TRUE(encoded);
    EXPECT_EQ(*encoded, "bnrc=4PCog3-12EcS");
}

TEST(AccessTraceStateTest, AddsEmptyBnrcForPreservedVendorState) {
    fiber::mem::BufPool pool;
    fiber::access_server::AccessTraceState state(pool);

    state.parse("vendor=opaque");

    auto encoded = state.encode();
    ASSERT_TRUE(encoded);
    EXPECT_EQ(*encoded, "vendor=opaque,bnrc=");
}

TEST(AccessTraceStateTest, LeavesMalformedInputForRawHeaderFallback) {
    fiber::mem::BufPool pool;
    fiber::access_server::AccessTraceState state(pool);

    state.parse("vendor=opaque,malformed");

    EXPECT_FALSE(state.should_override_upstream());
    auto encoded = state.encode();
    ASSERT_TRUE(encoded);
    EXPECT_TRUE(encoded->empty());
}

} // namespace
