#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "http/CookieCodec.h"

using fiber::http::Cookie;
using fiber::http::CookieSameSite;
using fiber::http::decode_cookie_header;
using fiber::http::encode_set_cookie;

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

} // namespace

TEST(CookieCodecTest, DecodesPairs) {
    auto pairs = decode("a=1; b=2; c=3");
    ASSERT_EQ(pairs.size(), 3u);
    EXPECT_EQ(pairs[0].name, "a");
    EXPECT_EQ(pairs[0].value, "1");
    EXPECT_EQ(pairs[1].name, "b");
    EXPECT_EQ(pairs[1].value, "2");
    EXPECT_EQ(pairs[2].name, "c");
    EXPECT_EQ(pairs[2].value, "3");
}

TEST(CookieCodecTest, DecodesWithoutEquals) {
    auto pairs = decode("flag; name=value");
    ASSERT_EQ(pairs.size(), 2u);
    EXPECT_EQ(pairs[0].name, "flag");
    EXPECT_EQ(pairs[0].value, "");
    EXPECT_EQ(pairs[1].name, "name");
    EXPECT_EQ(pairs[1].value, "value");
}

TEST(CookieCodecTest, DecodesTrimsWhitespaceAroundSeparators) {
    // Whitespace around the ';' separators is trimmed; spaces around '=' are not (a cookie
    // pair has no such spaces in practice).
    auto pairs = decode("a=1 ;  b=2 ");
    ASSERT_EQ(pairs.size(), 2u);
    EXPECT_EQ(pairs[0].name, "a");
    EXPECT_EQ(pairs[0].value, "1");
    EXPECT_EQ(pairs[1].name, "b");
    EXPECT_EQ(pairs[1].value, "2");
}

TEST(CookieCodecTest, DecodeSkipsEmptySegments) {
    auto pairs = decode(";;a=1;;;");
    ASSERT_EQ(pairs.size(), 1u);
    EXPECT_EQ(pairs[0].name, "a");
}

TEST(CookieCodecTest, EncodeMinimal) {
    Cookie c{};
    c.name = "sid";
    c.value = "abc";
    std::string out;
    ASSERT_TRUE(encode_set_cookie(c, out));
    EXPECT_EQ(out, "sid=abc");
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
    std::string out;
    ASSERT_TRUE(encode_set_cookie(c, out));
    EXPECT_EQ(out, "sid=abc; Domain=example.com; Path=/; Max-Age=3600; Secure; HttpOnly; SameSite=Lax");
}

TEST(CookieCodecTest, EncodeSessionCookieOmitsMaxAge) {
    Cookie c{};
    c.name = "sid";
    c.value = "v";
    c.max_age = -1; // session cookie
    std::string out;
    ASSERT_TRUE(encode_set_cookie(c, out));
    EXPECT_EQ(out, "sid=v");
}

TEST(CookieCodecTest, EncodeRejectsEmptyName) {
    Cookie c{};
    c.name = "";
    c.value = "v";
    std::string out;
    EXPECT_FALSE(encode_set_cookie(c, out));
}

TEST(CookieCodecTest, EncodeRejectsInvalidNameChar) {
    Cookie c{};
    c.name = "bad name"; // space is not a token char
    c.value = "v";
    std::string out;
    EXPECT_FALSE(encode_set_cookie(c, out));
}

TEST(CookieCodecTest, EncodeQuotesSpecialValueChars) {
    Cookie c{};
    c.name = "sid";
    c.value = "a b;c";
    std::string out;
    ASSERT_TRUE(encode_set_cookie(c, out));
    EXPECT_EQ(out, "sid=\"a b;c\"");
}

TEST(CookieCodecTest, EncodeSameSiteVariants) {
    for (auto [ss, text]: std::initializer_list<std::pair<CookieSameSite, const char *>>{
                 {CookieSameSite::None, "None"}, {CookieSameSite::Lax, "Lax"}, {CookieSameSite::Strict, "Strict"}}) {
        Cookie c{};
        c.name = "sid";
        c.value = "v";
        c.same_site = ss;
        std::string out;
        ASSERT_TRUE(encode_set_cookie(c, out));
        EXPECT_EQ(out, std::string("sid=v; SameSite=") + text);
    }
}

TEST(CookieCodecTest, EncodeZeroMaxAge) {
    Cookie c{};
    c.name = "del";
    c.value = "v";
    c.max_age = 0;
    std::string out;
    ASSERT_TRUE(encode_set_cookie(c, out));
    EXPECT_EQ(out, "del=v; Max-Age=0");
}
