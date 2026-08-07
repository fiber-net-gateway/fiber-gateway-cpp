#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fiber/common/mem/BufPool.h>
#include <fiber/http/HttpHeaderHash.h>
#include <fiber/http/HttpHeaders.h>

namespace {

using fiber::http::HttpHeaders;

static_assert(noexcept(HttpHeaders(std::declval<fiber::mem::BufPool &>())));
static_assert(noexcept(std::declval<HttpHeaders &>().pool()));
static_assert(noexcept(std::declval<HttpHeaders &>().add({}, {})));
static_assert(noexcept(std::declval<HttpHeaders &>().set({}, {})));
static_assert(noexcept(std::declval<HttpHeaders &>().add_view({}, {})));
static_assert(noexcept(std::declval<HttpHeaders &>().set_view({}, {})));
static_assert(noexcept(++std::declval<HttpHeaders::MatchIterator &>()));
static_assert(noexcept(++std::declval<HttpHeaders::ConstIterator &>()));

TEST(HttpHeadersTest, ExposesConstructionPoolForPoolBackedViews) {
    fiber::mem::BufPool pool;
    HttpHeaders headers(pool);
    constexpr std::string_view value = "Bearer provider-token";

    EXPECT_EQ(&headers.pool(), &pool);
    char *storage = headers.pool().alloc<char>(value.size());
    ASSERT_NE(storage, nullptr);
    std::memcpy(storage, value.data(), value.size());

    constexpr std::string_view lowcase_name = "authorization";
    constexpr uint64_t hash = fiber::http::http_header_name_hash(lowcase_name);
    ASSERT_NE(headers.set_view("Authorization", std::string_view(storage, value.size()), lowcase_name.data(), hash),
              nullptr);
    EXPECT_EQ(headers.get(lowcase_name, hash), value);
}

TEST(HttpHeadersTest, OwnedNameStoresOriginalAndLowercaseInOneBuffer) {
    fiber::mem::BufPool pool;
    HttpHeaders headers(pool);

    HttpHeaders::HeaderField *field = headers.add("Content-Type", "text/plain");
    ASSERT_NE(field, nullptr);
    EXPECT_EQ(field->name_view(), "Content-Type");
    EXPECT_EQ(field->lowcase_view(), "content-type");
    EXPECT_EQ(field->lowcase_name, field->name + field->name_len);
}

TEST(HttpHeadersTest, SetReusesPreparedLowercaseKeyToReplaceAllCaseVariants) {
    fiber::mem::BufPool pool;
    HttpHeaders headers(pool);
    ASSERT_NE(headers.add("Content-Type", "first"), nullptr);
    ASSERT_NE(headers.add("content-type", "second"), nullptr);
    ASSERT_NE(headers.add("other", "kept"), nullptr);

    HttpHeaders::HeaderField *field = headers.set("CONTENT-TYPE", "replacement");
    ASSERT_NE(field, nullptr);
    EXPECT_EQ(field->name_view(), "CONTENT-TYPE");
    EXPECT_EQ(field->lowcase_view(), "content-type");
    EXPECT_EQ(headers.get("content-type"), "replacement");
    EXPECT_EQ(headers.get("other"), "kept");
    EXPECT_EQ(headers.size(), 2u);
}

TEST(HttpHeadersTest, ConvenienceGetAllRemainsCaseInsensitive) {
    fiber::mem::BufPool pool;
    HttpHeaders headers(pool);
    ASSERT_NE(headers.add("X-Test", "1"), nullptr);
    ASSERT_NE(headers.add("x-test", "2"), nullptr);
    ASSERT_NE(headers.add("other", "3"), nullptr);

    std::vector<std::string_view> values;
    for (const auto &field: headers.get_all("X-TEST")) {
        values.push_back(field.value_view());
    }

    EXPECT_EQ((std::vector<std::string_view>{"2", "1"}), values);
}

TEST(HttpHeadersTest, PrehashedOwnedAddBuildsLongLowercaseName) {
    fiber::mem::BufPool pool;
    HttpHeaders headers(pool);
    const std::string name = "X-Very-Long-Trailer-Name-That-Exceeds-Parser-Cache";
    std::string lowcase(name.size(), '\0');
    fiber::http::to_lowercase(name, lowcase.data());
    const uint64_t hash = fiber::http::http_header_name_hash(name);

    HttpHeaders::HeaderField *field = headers.add_prehashed(name, "value", hash);
    ASSERT_NE(field, nullptr);
    EXPECT_EQ(field->name_view(), name);
    EXPECT_EQ(field->lowcase_view(), lowcase);
    EXPECT_EQ(field->name_hash, hash);
    EXPECT_EQ(headers.get(lowcase, hash), "value");
}

TEST(HttpHeadersTest, RehashPreservesBucketAndInsertionOrderLinks) {
    fiber::mem::BufPool pool;
    HttpHeaders headers(pool);
    constexpr size_t kFieldCount = 96;

    for (size_t i = 0; i < kFieldCount; ++i) {
        const std::string name = "X-Field-" + std::to_string(i);
        const std::string value = std::to_string(i);
        ASSERT_NE(headers.add(name, value), nullptr);
    }

    EXPECT_EQ(headers.size(), kFieldCount);
    size_t index = 0;
    for (const auto &field: headers) {
        EXPECT_EQ(field.name_view(), "X-Field-" + std::to_string(index));
        ++index;
    }
    EXPECT_EQ(index, kFieldCount);

    for (size_t i = 0; i < kFieldCount; ++i) {
        const std::string name = "x-field-" + std::to_string(i);
        EXPECT_EQ(headers.get(name), std::to_string(i));
    }

    headers.clear();
    EXPECT_EQ(headers.size(), 0u);
    ASSERT_NE(headers.add("after-clear", "1"), nullptr);
    EXPECT_EQ(headers.get("after-clear"), "1");

    headers.release();
    EXPECT_EQ(headers.size(), 0u);
    ASSERT_NE(headers.add("after-release", "2"), nullptr);
    EXPECT_EQ(headers.get("after-release"), "2");
}

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
