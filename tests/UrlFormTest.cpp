#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "common/IoError.h"
#include "common/url/UrlForm.h"

namespace {

using fiber::common::IoErr;
using fiber::common::url::form_build_query;
using fiber::common::url::form_decode;
using fiber::common::url::form_decode_into;
using fiber::common::url::form_decode_query;
using fiber::common::url::form_encode;

std::string encode(std::string_view in) {
    std::string out;
    form_encode(in, out);
    return out;
}

std::vector<std::pair<std::string, std::string>> decode_query(std::string_view in) {
    std::vector<std::pair<std::string, std::string>> pairs;
    auto io = form_decode_query(in, [&](std::string_view k, std::string_view v) {
        pairs.emplace_back(std::string(k), std::string(v));
        return true;
    });
    EXPECT_TRUE(io.has_value()) << "expected query to decode";
    return pairs;
}

struct VecSource {
    const std::vector<std::pair<std::string, std::string>> &pairs;
    std::size_t i = 0;
    bool operator()(std::string &key, std::string &value) {
        if (i >= pairs.size()) {
            return false;
        }
        key = pairs[i].first;
        value = pairs[i].second;
        ++i;
        return true;
    }
};

std::string build_query(const std::vector<std::pair<std::string, std::string>> &pairs) {
    std::string out;
    VecSource src{pairs, 0};
    form_build_query(out, src);
    return out;
}

// ---- form_encode ----

TEST(UrlFormTest, EncodeSpacesAndUnreserved) {
    EXPECT_EQ(encode("a b"), "a+b");
    EXPECT_EQ(encode("a*b-c.d_e"), "a*b-c.d_e");
    EXPECT_EQ(encode("ABCxyz09"), "ABCxyz09");
    EXPECT_EQ(encode(" "), "+");
    EXPECT_EQ(encode(""), "");
}

TEST(UrlFormTest, EncodeUppercaseHexForNonUnreserved) {
    EXPECT_EQ(encode("100%"), "100%25");
    EXPECT_EQ(encode("<>&"), "%3C%3E%26");
    // 'e' with acute (U+00E9) is UTF-8 C3 A9 -> escaped per byte, uppercase.
    EXPECT_EQ(encode("\xc3\xa9"), "%C3%A9");
}

// ---- form_decode ----

TEST(UrlFormTest, DecodePlusAndPercent) {
    EXPECT_EQ(form_decode("a+b").value(), "a b");
    EXPECT_EQ(form_decode("a%20b").value(), "a b");
    EXPECT_EQ(form_decode("100%25").value(), "100%");
    EXPECT_EQ(form_decode("\xc3\xa9").value(), "\xc3\xa9"); // literal pass-through
    EXPECT_EQ(form_decode("%C3%A9").value(), "\xc3\xa9"); // percent-encoded -> decoded
    EXPECT_EQ(form_decode("a%26b").value(), "a&b");
    EXPECT_EQ(form_decode("+%2B+").value(), " + ");
    EXPECT_EQ(form_decode("").value(), "");
}

TEST(UrlFormTest, DecodeMalformedEscapeIsInvalid) {
    EXPECT_EQ(form_decode("%ZZ").error(), IoErr::Invalid);
    EXPECT_EQ(form_decode("%1").error(), IoErr::Invalid);
    EXPECT_EQ(form_decode("%").error(), IoErr::Invalid);
    EXPECT_EQ(form_decode("a%G0").error(), IoErr::Invalid);
}

TEST(UrlFormTest, DecodeMalformedUtf8IsReplacedWithReplacementChar) {
    // Lone continuation byte -> U+FFFD (EF BF BD).
    EXPECT_EQ(form_decode("%FF").value(), "\xef\xbf\xbd");
    // Incomplete multi-byte sequence -> U+FFFD.
    EXPECT_EQ(form_decode("%C3").value(), "\xef\xbf\xbd");
    // Valid char followed by an incomplete byte -> char + U+FFFD.
    EXPECT_EQ(form_decode("%C3%A9%C3").value(), "\xc3\xa9\xef\xbf\xbd");
}

TEST(UrlFormTest, DecodeIntoReturnsFalseOnMalformed) {
    std::string out;
    EXPECT_FALSE(form_decode_into("%ZZ", out));
    EXPECT_TRUE(form_decode_into("a+b", out));
    EXPECT_EQ(out, "a b");
}

// ---- form_decode_query ----

TEST(UrlFormTest, ParseQueryRepeatedKeyAggregatesInOrder) {
    auto pairs = decode_query("a=1&a=2&b=x");
    ASSERT_EQ(pairs.size(), 3u);
    EXPECT_EQ(pairs[0], (std::pair{"a", "1"}));
    EXPECT_EQ(pairs[1], (std::pair{"a", "2"}));
    EXPECT_EQ(pairs[2], (std::pair{"b", "x"}));
}

TEST(UrlFormTest, ParseQueryEdgeSegments) {
    EXPECT_TRUE(decode_query("").empty());
    auto noEq = decode_query("a");
    ASSERT_EQ(noEq.size(), 1u);
    EXPECT_EQ(noEq[0], (std::pair{"a", ""}));
    auto trailingEq = decode_query("a=");
    ASSERT_EQ(trailingEq.size(), 1u);
    EXPECT_EQ(trailingEq[0], (std::pair{"a", ""}));
    auto leadingEq = decode_query("=x");
    ASSERT_EQ(leadingEq.size(), 1u);
    EXPECT_EQ(leadingEq[0], (std::pair{"", "x"}));
    // '=' after the first is part of the value.
    auto eqInValue = decode_query("a=b=c");
    ASSERT_EQ(eqInValue.size(), 1u);
    EXPECT_EQ(eqInValue[0], (std::pair{"a", "b=c"}));
    // Empty segments are skipped.
    auto emptySegs = decode_query("a=1&&b=2");
    ASSERT_EQ(emptySegs.size(), 2u);
    EXPECT_EQ(emptySegs[0], (std::pair{"a", "1"}));
    EXPECT_EQ(emptySegs[1], (std::pair{"b", "2"}));
}

TEST(UrlFormTest, ParseQueryDecodesKeysAndValues) {
    auto pairs = decode_query("%C3%A9=x&k=a+b");
    ASSERT_EQ(pairs.size(), 2u);
    EXPECT_EQ(pairs[0], (std::pair{"\xc3\xa9", "x"}));
    EXPECT_EQ(pairs[1], (std::pair{"k", "a b"}));
    // Malformed UTF-8 in a value is replaced.
    auto repaired = decode_query("a=%FF");
    ASSERT_EQ(repaired.size(), 1u);
    EXPECT_EQ(repaired[0], (std::pair{"a", "\xef\xbf\xbd"}));
    // A lone '+' decodes to a space key with empty value.
    auto plus = decode_query("+");
    ASSERT_EQ(plus.size(), 1u);
    EXPECT_EQ(plus[0], (std::pair{" ", ""}));
}

TEST(UrlFormTest, ParseQueryMalformedEscapeIsInvalid) {
    std::vector<std::pair<std::string, std::string>> pairs;
    auto io = form_decode_query("%ZZ=1", [&](std::string_view, std::string_view) {
        pairs.emplace_back();
        return true;
    });
    ASSERT_FALSE(io.has_value());
    EXPECT_EQ(io.error(), IoErr::Invalid);
    EXPECT_TRUE(pairs.empty());
}

TEST(UrlFormTest, ParseQuerySinkAbortStopsEarly) {
    std::vector<std::pair<std::string, std::string>> pairs;
    auto io = form_decode_query("a=1&b=2&c=3", [&](std::string_view k, std::string_view v) {
        pairs.emplace_back(std::string(k), std::string(v));
        return pairs.size() < 2; // stop after two pairs
    });
    EXPECT_FALSE(io.has_value());
    EXPECT_EQ(pairs.size(), 2u);
}

// ---- form_build_query ----

TEST(UrlFormTest, BuildQueryJoinsPairs) {
    EXPECT_EQ(build_query({{"a", "1"}, {"b", "x"}}), "a=1&b=x");
    EXPECT_EQ(build_query({}), "");
    EXPECT_EQ(build_query({{"a", "1"}}), "a=1");
}

TEST(UrlFormTest, BuildQueryEncodesKeysAndValues) {
    EXPECT_EQ(build_query({{"a b", "c d"}}), "a+b=c+d");
    EXPECT_EQ(build_query({{"\xc3\xa9", "x"}}), "%C3%A9=x");
    EXPECT_EQ(build_query({{"a&b", "c=d"}}), "a%26b=c%3Dd");
}

} // namespace
