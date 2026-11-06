#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <fiber/common/util/CookieCodec.h>

using fiber::util::Cookie;
using fiber::util::CookieSameSite;
using fiber::util::decode_cookie_header;
using fiber::util::encode_set_cookie;

// noexcept is conditional on the sink, mirroring encode_set_cookie's allocator.
static_assert(noexcept(decode_cookie_header(std::string_view{}, [](std::string_view, std::string_view) noexcept {})));
static_assert(!noexcept(decode_cookie_header(std::string_view{}, [](std::string_view, std::string_view) {})));

namespace {

struct Pair {
    std::string name;
    std::string value;
};

std::vector<Pair> decode(std::string_view header) {
    std::vector<Pair> out;
    decode_cookie_header(header, [&](std::string_view name, std::string_view value) {
        out.push_back({std::string(name), std::string(value)});
    });
    return out;
}

// Encodes into a std::string-backed buffer; "" means the cookie was rejected (a valid
// cookie always encodes to at least "n=", so empty is unambiguous).
std::string encode(const Cookie &c) {
    std::string out;
    const std::string_view sv = encode_set_cookie(c, [&](std::size_t n) -> char * {
        out.resize(n);
        return out.data();
    });
    return std::string(sv);
}

} // namespace

TEST(CookieCodecTest, DecodesPairs) {
    auto pairs = decode("a=1; b=2; c=3");
    ASSERT_EQ(pairs.size(), 3U);
    EXPECT_EQ(pairs[0].name, "a");
    EXPECT_EQ(pairs[0].value, "1");
    EXPECT_EQ(pairs[1].name, "b");
    EXPECT_EQ(pairs[1].value, "2");
    EXPECT_EQ(pairs[2].name, "c");
    EXPECT_EQ(pairs[2].value, "3");
}

TEST(CookieCodecTest, DecodesWithoutEquals) {
    auto pairs = decode("flag; name=value");
    ASSERT_EQ(pairs.size(), 2U);
    EXPECT_EQ(pairs[0].name, "flag");
    EXPECT_EQ(pairs[0].value, "");
    EXPECT_EQ(pairs[1].name, "name");
    EXPECT_EQ(pairs[1].value, "value");
}

TEST(CookieCodecTest, DecodesTrimsWhitespaceAroundSeparators) {
    // Whitespace around the ';' separators is trimmed; spaces around '=' are not (a cookie
    // pair has no such spaces in practice).
    auto pairs = decode("a=1 ;  b=2 ");
    ASSERT_EQ(pairs.size(), 2U);
    EXPECT_EQ(pairs[0].name, "a");
    EXPECT_EQ(pairs[0].value, "1");
    EXPECT_EQ(pairs[1].name, "b");
    EXPECT_EQ(pairs[1].value, "2");
}

TEST(CookieCodecTest, DecodeSkipsEmptySegments) {
    auto pairs = decode(";;a=1;;;");
    ASSERT_EQ(pairs.size(), 1U);
    EXPECT_EQ(pairs[0].name, "a");
}

TEST(CookieCodecTest, EncodeMinimal) {
    Cookie c{};
    c.name = "sid";
    c.value = "abc";
    EXPECT_EQ(encode(c), "sid=abc");
}

TEST(CookieCodecTest, EncodeFullAttributes) {
    Cookie c{};
    c.name = "sid";
    c.value = "abc";
    c.domain = "example.com";
    c.path = "/";
    c.max_age = 3600;
    c.secure = true;
    c.http_only = true;
    c.same_site = CookieSameSite::Lax;
    EXPECT_EQ(encode(c), "sid=abc; Domain=example.com; Path=/; Max-Age=3600; Secure; HttpOnly; SameSite=Lax");
}

TEST(CookieCodecTest, EncodeSessionCookieOmitsMaxAge) {
    Cookie c{};
    c.name = "sid";
    c.value = "v";
    c.max_age = -1; // session cookie
    EXPECT_EQ(encode(c), "sid=v");
}

TEST(CookieCodecTest, EncodeRejectsEmptyName) {
    Cookie c{};
    c.name = "";
    c.value = "v";
    EXPECT_EQ(encode(c), "");
}

TEST(CookieCodecTest, EncodeRejectsInvalidNameChar) {
    Cookie c{};
    c.name = "bad name"; // space is not a token char
    c.value = "v";
    EXPECT_EQ(encode(c), "");
}

TEST(CookieCodecTest, EncodeQuotesSpecialValueChars) {
    Cookie c{};
    c.name = "sid";
    c.value = "a b;c";
    EXPECT_EQ(encode(c), "sid=\"a b;c\"");
}

TEST(CookieCodecTest, EncodeEscapesQuoteAndBackslashInsideQuotes) {
    Cookie c{};
    c.name = "sid";
    c.value = "a\"b\\c";
    EXPECT_EQ(encode(c), "sid=\"a\\\"b\\\\c\"");
}

TEST(CookieCodecTest, EncodeSameSiteVariants) {
    for (auto [ss, text]: std::initializer_list<std::pair<CookieSameSite, const char *>>{
                 {CookieSameSite::None, "None"}, {CookieSameSite::Lax, "Lax"}, {CookieSameSite::Strict, "Strict"}}) {
        Cookie c{};
        c.name = "sid";
        c.value = "v";
        c.same_site = ss;
        EXPECT_EQ(encode(c), std::string("sid=v; SameSite=") + text);
    }
}

TEST(CookieCodecTest, EncodeZeroMaxAge) {
    Cookie c{};
    c.name = "del";
    c.value = "v";
    c.max_age = 0;
    EXPECT_EQ(encode(c), "del=v; Max-Age=0");
}

TEST(CookieCodecTest, EncodeAllocatesOnceWithExactSizeAndAliasesCallerBuffer) {
    Cookie c{};
    c.name = "sid";
    c.value = "a b;c"; // quoted: 7 bytes encoded
    c.path = "/";
    c.max_age = 7;
    std::size_t requested = 0;
    char buf[128];
    std::string_view sv = encode_set_cookie(c, [&](std::size_t n) noexcept -> char * {
        requested = n;
        return n <= sizeof(buf) ? buf : nullptr;
    });
    EXPECT_EQ(requested, sv.size()) << "allocator must receive the exact encoded size";
    EXPECT_EQ(sv.data(), buf) << "returned view must alias the caller buffer";
    EXPECT_EQ(sv, "sid=\"a b;c\"; Path=/; Max-Age=7");
}

TEST(CookieCodecTest, EncodeRefusedAllocationYieldsEmptyView) {
    Cookie c{};
    c.name = "sid";
    c.value = "abc";
    const std::string_view sv = encode_set_cookie(c, [](std::size_t) noexcept -> char * { return nullptr; });
    EXPECT_TRUE(sv.empty());
}
