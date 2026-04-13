#include <gtest/gtest.h>

#include "http/HeaderMap.h"
#include "http/HttpHeaderHash.h"

TEST(HeaderMapTest, MatchesCaseInsensitiveLookupsAndCachedHash) {
    fiber::http::HeaderMap<int> map;
    ASSERT_TRUE(map.insert("content-length", 42));

    const auto cached_hash = fiber::http::http_header_name_hash("Content-Length");

    const int *mixed_case = map.get("Content-Length");
    ASSERT_NE(mixed_case, nullptr);
    EXPECT_EQ(*mixed_case, 42);

    const int *cached = map.get("content-length", cached_hash);
    ASSERT_NE(cached, nullptr);
    EXPECT_EQ(*cached, 42);
}

TEST(HeaderMapTest, RejectsDuplicateKeysWhenUsingCachedHash) {
    fiber::http::HeaderMap<int> map;
    ASSERT_TRUE(map.insert("connection", fiber::http::http_header_name_hash("connection"), 1));
    EXPECT_FALSE(map.insert("Connection", fiber::http::http_header_name_hash("Connection"), 2));
    EXPECT_EQ(map.size(), 1U);
}
