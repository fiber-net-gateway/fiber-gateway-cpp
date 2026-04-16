#include <gtest/gtest.h>

#include <string_view>
#include <vector>

#include "common/mem/BufPool.h"
#include "http/HttpHeaderHash.h"
#include "http/HttpHeaders.h"

namespace {

using fiber::http::HttpHeaders;

TEST(HttpHeadersTest, EraseConstIteratorSupportsRemovalDuringFullTraversal) {
    fiber::mem::BufPool pool;
    HttpHeaders headers(pool);
    ASSERT_NE(headers.add("a", "1"), nullptr);
    ASSERT_NE(headers.add("b", "2"), nullptr);
    ASSERT_NE(headers.add("c", "3"), nullptr);
    ASSERT_NE(headers.add("d", "4"), nullptr);

    std::vector<std::string_view> kept;
    for (auto it = headers.begin(); it != headers.end();) {
        if (it->name_view() == "b" || it->name_view() == "d") {
            it = headers.erase(it);
            continue;
        }
        kept.push_back(it->name_view());
        ++it;
    }

    EXPECT_EQ((std::vector<std::string_view>{"a", "c"}), kept);
    EXPECT_EQ(headers.size(), 2u);
    EXPECT_FALSE(headers.contains("b"));
    EXPECT_FALSE(headers.contains("d"));
}

TEST(HttpHeadersTest, EraseMatchIteratorSupportsRemovalDuringFilteredTraversal) {
    fiber::mem::BufPool pool;
    HttpHeaders headers(pool);
    ASSERT_NE(headers.add("x-test", "1"), nullptr);
    ASSERT_NE(headers.add("other", "a"), nullptr);
    ASSERT_NE(headers.add("x-test", "2"), nullptr);
    ASSERT_NE(headers.add("x-test", "3"), nullptr);

    const std::string_view key = "x-test";
    const uint64_t hash = fiber::http::http_header_name_hash(key);

    size_t removed = 0;
    auto range = headers.get_all(key, hash);
    for (auto it = range.begin(); it != range.end();) {
        EXPECT_EQ(it->lowcase_view(), key);
        it = headers.erase(it);
        ++removed;
    }

    EXPECT_EQ(removed, 3u);
    EXPECT_EQ(headers.size(), 1u);
    EXPECT_FALSE(headers.contains("x-test"));
    EXPECT_TRUE(headers.contains("other"));
}

TEST(HttpHeadersTest, RemoveByLowercaseNameAndHashUsesCachedLookup) {
    fiber::mem::BufPool pool;
    HttpHeaders headers(pool);
    ASSERT_NE(headers.add("Content-Type", "text/plain"), nullptr);
    ASSERT_NE(headers.add("content-type", "application/json"), nullptr);
    ASSERT_NE(headers.add("Host", "example.com"), nullptr);

    const std::string_view key = "content-type";
    const uint64_t hash = fiber::http::http_header_name_hash(key);

    EXPECT_EQ(headers.remove(key, hash), 2u);
    EXPECT_EQ(headers.size(), 1u);
    EXPECT_FALSE(headers.contains("content-type"));
    EXPECT_TRUE(headers.contains("host"));
}

TEST(HttpHeadersTest, RemoveHeaderFieldDeletesOnlyThatNode) {
    fiber::mem::BufPool pool;
    HttpHeaders headers(pool);
    HttpHeaders::HeaderField *first = headers.add("set-cookie", "a=1");
    HttpHeaders::HeaderField *second = headers.add("set-cookie", "b=2");
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    EXPECT_TRUE(headers.remove(*second));
    EXPECT_EQ(headers.size(), 1u);
    EXPECT_EQ(headers.get("set-cookie"), "a=1");
    EXPECT_FALSE(headers.remove(*second));
    EXPECT_TRUE(headers.remove(*first));
    EXPECT_EQ(headers.size(), 0u);
}

} // namespace
