#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fiber/common/IoError.h>
#include <fiber/common/json/Utf.h>
#include <fiber/common/mem/BufPool.h>
#include <fiber/common/util/UrlForm.h>

#include "support/AllocFailure.h"

namespace {

using fiber::common::IoErr;
using fiber::util::form_build_query;
using fiber::util::form_decode;
using fiber::util::form_decode_into;
using fiber::util::form_decode_query;
using fiber::util::form_encode;
using fiber::util::form_percent_decode;

using QueryPair = std::pair<std::string, std::string>;

std::string encode(std::string_view in) {
    std::string out;
    form_encode(in, out);
    return out;
}

// The buffer source the script callers use.
auto pool_alloc(fiber::mem::BufPool &pool) {
    return [&pool](std::size_t size) noexcept { return pool.alloc<char>(size); };
}

std::vector<QueryPair> decode_query(std::string_view in) {
    fiber::mem::BufPool pool;
    std::vector<QueryPair> pairs;
    auto io = form_decode_query(in, pool_alloc(pool), [&](std::string_view k, std::string_view v) {
        pairs.emplace_back(std::string(k), std::string(v));
        return true;
    });
    EXPECT_TRUE(io.has_value()) << "expected query to decode";
    return pairs;
}

struct QueryOutcome {
    std::vector<QueryPair> pairs;
    bool ok = true;

    bool operator==(const QueryOutcome &) const = default;
};

// The documented split, one form_decode per component: what form_decode_query must match.
QueryOutcome reference_decode_query(std::string_view in) {
    QueryOutcome outcome;
    std::size_t pos = 0;
    while (pos <= in.size()) {
        const std::size_t amp = in.find('&', pos);
        const std::size_t end = amp == std::string_view::npos ? in.size() : amp;
        const std::string_view seg = in.substr(pos, end - pos);
        const std::size_t eq = seg.find('=');
        auto key = form_decode(seg.substr(0, eq));
        if (!key) {
            outcome.ok = false;
            return outcome;
        }
        if (eq != std::string_view::npos) {
            auto value = form_decode(seg.substr(eq + 1));
            if (!value) {
                outcome.ok = false;
                return outcome;
            }
            outcome.pairs.emplace_back(std::move(*key), std::move(*value));
        } else if (!key->empty()) {
            outcome.pairs.emplace_back(std::move(*key), std::string());
        }
        pos = end + 1;
    }
    return outcome;
}

// Every buffer is its own exact-size heap block, so a sanitizer build catches any overrun.
QueryOutcome exact_decode_query(std::string_view in) {
    std::vector<std::unique_ptr<char[]>> buffers;
    QueryOutcome outcome;
    auto io = form_decode_query(
            in,
            [&](std::size_t size) {
                buffers.push_back(std::make_unique<char[]>(size));
                return buffers.back().get();
            },
            [&](std::string_view k, std::string_view v) {
                outcome.pairs.emplace_back(std::string(k), std::string(v));
                return true;
            });
    outcome.ok = io.has_value();
    return outcome;
}

struct RepairCase {
    std::string_view in;
    std::string_view expected;
};

constexpr RepairCase kRepairCases[] = {
        {.in = "", .expected = ""},
        {.in = "plain", .expected = "plain"},
        {.in = "\xc3\xa9", .expected = "\xc3\xa9"},
        {.in = "\xf0\x9f\x98\x80", .expected = "\xf0\x9f\x98\x80"},
        {.in = "\xff", .expected = "\xef\xbf\xbd"},
        {.in = "a\xff"
               "b",
         .expected = "a\xef\xbf\xbd"
                     "b"},
        {.in = "\xc3", .expected = "\xef\xbf\xbd"}, // truncated sequence
        {.in = "\xe4\xbd", .expected = "\xef\xbf\xbd\xef\xbf\xbd"}, // one U+FFFD per byte
        {.in = "\xc0\x80", .expected = "\xef\xbf\xbd\xef\xbf\xbd"}, // overlong
        {.in = "\xed\xa0\x80", .expected = "\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd"}, // surrogate
        {.in = "\xf0\x9f\x98\x80\xff", .expected = "\xf0\x9f\x98\x80\xef\xbf\xbd"},
};

struct VecSource {
    const std::vector<QueryPair> &pairs;
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

std::string build_query(const std::vector<QueryPair> &pairs) {
    std::string out;
    VecSource src{.pairs = pairs, .i = 0};
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
    std::string out = "stale";
    EXPECT_FALSE(form_decode_into("%ZZ", out));
    EXPECT_TRUE(out.empty());
    EXPECT_TRUE(form_decode_into("a+b", out));
    EXPECT_EQ(out, "a b");
}

TEST(UrlFormTest, PercentDecodeLeavesUtf8ToTheRepairPhase) {
    char dst[8];
    std::size_t len = 0;
    ASSERT_TRUE(form_percent_decode("a+%FF%41", dst, len));
    EXPECT_EQ(std::string_view(dst, len), "a \xff"
                                          "A");
    EXPECT_FALSE(form_percent_decode("%ZZ", dst, len));
    EXPECT_FALSE(form_percent_decode("%1", dst, len));
}

TEST(UrlFormTest, Utf8RepairWritesExactlyItsSize) {
    for (const RepairCase &repair: kRepairCases) {
        const std::size_t size = fiber::json::utf8_repaired_size(repair.in);
        ASSERT_EQ(size, repair.expected.size()) << repair.in;
        std::string out(size + 1, '#');
        fiber::json::utf8_repair_into(repair.in, out.data());
        EXPECT_EQ(std::string_view(out).substr(0, size), repair.expected);
        EXPECT_EQ(out.back(), '#') << repair.in;
    }
}

// Each failed allocation must leave form_decode as bad_alloc: the script host functions
// catch it in oom_guard, and a noexcept helper below them would terminate first.
TEST(UrlFormTest, DecodeOomReachesTheCaller) {
    if constexpr (!fiber::test::kAllocFailureAvailable) {
        GTEST_SKIP() << "no operator new replacement in this build";
    }
    std::string input;
    for (int i = 0; i < 64; ++i) {
        input += "%FF";
    }
    std::size_t failures = 0;
    for (std::size_t successes = 0;; ++successes) {
        try {
            fiber::test::ScopedAllocFailure out_of_memory(successes);
            auto decoded = form_decode(input);
            break;
        } catch (const std::bad_alloc &) {
            ++failures;
        }
    }
    EXPECT_GE(failures, 2U); // the decode buffer and the repair buffer each failed once
}

// ---- form_decode_query ----

TEST(UrlFormTest, ParseQueryRepeatedKeyAggregatesInOrder) {
    auto pairs = decode_query("a=1&a=2&b=x");
    ASSERT_EQ(pairs.size(), 3U);
    EXPECT_EQ(pairs[0], (QueryPair{"a", "1"}));
    EXPECT_EQ(pairs[1], (QueryPair{"a", "2"}));
    EXPECT_EQ(pairs[2], (QueryPair{"b", "x"}));
}

TEST(UrlFormTest, ParseQueryEdgeSegments) {
    EXPECT_TRUE(decode_query("").empty());
    auto noEq = decode_query("a");
    ASSERT_EQ(noEq.size(), 1U);
    EXPECT_EQ(noEq[0], (QueryPair{"a", ""}));
    auto trailingEq = decode_query("a=");
    ASSERT_EQ(trailingEq.size(), 1U);
    EXPECT_EQ(trailingEq[0], (QueryPair{"a", ""}));
    auto leadingEq = decode_query("=x");
    ASSERT_EQ(leadingEq.size(), 1U);
    EXPECT_EQ(leadingEq[0], (QueryPair{"", "x"}));
    // '=' after the first is part of the value.
    auto eqInValue = decode_query("a=b=c");
    ASSERT_EQ(eqInValue.size(), 1U);
    EXPECT_EQ(eqInValue[0], (QueryPair{"a", "b=c"}));
    // Empty segments are skipped.
    auto emptySegs = decode_query("a=1&&b=2");
    ASSERT_EQ(emptySegs.size(), 2U);
    EXPECT_EQ(emptySegs[0], (QueryPair{"a", "1"}));
    EXPECT_EQ(emptySegs[1], (QueryPair{"b", "2"}));
}

TEST(UrlFormTest, ParseQueryDecodesKeysAndValues) {
    auto pairs = decode_query("%C3%A9=x&k=a+b");
    ASSERT_EQ(pairs.size(), 2U);
    EXPECT_EQ(pairs[0], (QueryPair{"\xc3\xa9", "x"}));
    EXPECT_EQ(pairs[1], (QueryPair{"k", "a b"}));
    // Malformed UTF-8 in a value is replaced.
    auto repaired = decode_query("a=%FF");
    ASSERT_EQ(repaired.size(), 1U);
    EXPECT_EQ(repaired[0], (QueryPair{"a", "\xef\xbf\xbd"}));
    // A lone '+' decodes to a space key with empty value.
    auto plus = decode_query("+");
    ASSERT_EQ(plus.size(), 1U);
    EXPECT_EQ(plus[0], (QueryPair{" ", ""}));
}

TEST(UrlFormTest, ParseQueryMalformedEscapeIsInvalid) {
    fiber::mem::BufPool pool;
    std::vector<std::pair<std::string, std::string>> pairs;
    auto io = form_decode_query("%ZZ=1", pool_alloc(pool), [&](std::string_view, std::string_view) {
        pairs.emplace_back();
        return true;
    });
    ASSERT_FALSE(io.has_value());
    EXPECT_EQ(io.error(), IoErr::Invalid);
    EXPECT_TRUE(pairs.empty());
}

TEST(UrlFormTest, ParseQuerySinkAbortStopsEarly) {
    fiber::mem::BufPool pool;
    std::vector<std::pair<std::string, std::string>> pairs;
    auto io = form_decode_query("a=1&b=2&c=3", pool_alloc(pool), [&](std::string_view k, std::string_view v) {
        pairs.emplace_back(std::string(k), std::string(v));
        return pairs.size() < 2; // stop after two pairs
    });
    EXPECT_FALSE(io.has_value());
    EXPECT_EQ(pairs.size(), 2U);
}

TEST(UrlFormTest, ParseQueryMalformedEscapeKeepsEarlierPairs) {
    QueryOutcome outcome = exact_decode_query("a=1&b=%ZZ&c=3");
    EXPECT_FALSE(outcome.ok);
    ASSERT_EQ(outcome.pairs.size(), 1U);
    EXPECT_EQ(outcome.pairs[0], (QueryPair{"a", "1"}));
}

TEST(UrlFormTest, ParseQueryMatchesPerComponentDecode) {
    // Shapes that stress the shared buffer: empty parts, repair on either side of '=',
    // truncated sequences split across components, and malformed escapes.
    constexpr std::string_view kTokens[] = {"a",   "bc",  "=",   "&",         "+", "%41",
                                            "%FF", "%C3", "%A9", "%E4%BD%A0", "%", "%Z"};
    std::uint32_t state = 0x9E3779B9U;
    for (int round = 0; round < 20000; ++round) {
        std::string query;
        state = (state * 1664525U) + 1013904223U;
        const std::uint32_t tokens = (state >> 24) % 12;
        for (std::uint32_t i = 0; i < tokens; ++i) {
            state = (state * 1664525U) + 1013904223U;
            query += kTokens[(state >> 24) % std::size(kTokens)];
        }
        ASSERT_EQ(exact_decode_query(query), reference_decode_query(query)) << query;
    }
}

TEST(UrlFormTest, ParseQueryWellFormedTakesOneBuffer) {
    fiber::mem::BufPool pool;
    std::vector<std::size_t> sizes;
    constexpr std::string_view kQuery = "alpha=1&beta=%C3%A9&gamma&delta=";
    std::size_t pairs = 0;
    auto io = form_decode_query(
            kQuery,
            [&](std::size_t size) {
                sizes.push_back(size);
                return pool.alloc<char>(size);
            },
            [&](std::string_view, std::string_view) {
                ++pairs;
                return true;
            });
    ASSERT_TRUE(io.has_value());
    EXPECT_EQ(pairs, 4U);
    EXPECT_EQ(sizes, std::vector<std::size_t>{kQuery.size()});
}

TEST(UrlFormTest, ParseQueryEmptyInputTakesNoBuffer) {
    bool allocated = false;
    auto io = form_decode_query(
            "",
            [&](std::size_t) -> char * {
                allocated = true;
                return nullptr;
            },
            [](std::string_view, std::string_view) { return true; });
    EXPECT_TRUE(io.has_value());
    EXPECT_FALSE(allocated);
}

TEST(UrlFormTest, ParseQueryRepairTakesExactBuffers) {
    fiber::mem::BufPool pool;
    std::vector<std::size_t> sizes;
    std::vector<QueryPair> pairs;
    constexpr std::string_view kQuery = "%FF=a%FF"
                                        "b";
    auto io = form_decode_query(
            kQuery,
            [&](std::size_t size) {
                sizes.push_back(size);
                return pool.alloc<char>(size);
            },
            [&](std::string_view k, std::string_view v) {
                pairs.emplace_back(std::string(k), std::string(v));
                return true;
            });
    ASSERT_TRUE(io.has_value());
    EXPECT_EQ(sizes, (std::vector<std::size_t>{kQuery.size(), 3, 5}));
    ASSERT_EQ(pairs.size(), 1U);
    EXPECT_EQ(pairs[0], (QueryPair{"\xef\xbf\xbd", "a\xef\xbf\xbd"
                                                   "b"}));
}

TEST(UrlFormTest, ParseQueryRefusedBufferIsNoMem) {
    // Request 0 is the shared buffer, 1 the key's repair, 2 the value's repair.
    for (std::size_t refused = 0; refused < 3; ++refused) {
        fiber::mem::BufPool pool;
        std::size_t requests = 0;
        std::size_t pairs = 0;
        auto io = form_decode_query(
                "%FF=%FE", [&](std::size_t size) { return requests++ == refused ? nullptr : pool.alloc<char>(size); },
                [&](std::string_view, std::string_view) {
                    ++pairs;
                    return true;
                });
        ASSERT_FALSE(io.has_value()) << refused;
        EXPECT_EQ(io.error(), IoErr::NoMem) << refused;
        EXPECT_EQ(pairs, 0U) << refused;
    }
}

TEST(UrlFormTest, ParseQueryMakesNoThrowingAllocation) {
    if constexpr (!fiber::test::kAllocFailureAvailable) {
        GTEST_SKIP() << "no operator new replacement in this build";
    }
    char backing[64];
    std::size_t used = 0;
    auto alloc = [&](std::size_t size) noexcept {
        char *buffer = backing + used;
        used += size;
        return buffer;
    };
    std::size_t decoded_bytes = 0;
    auto sink = [&](std::string_view k, std::string_view v) noexcept {
        decoded_bytes += k.size() + v.size();
        return true;
    };
    static_assert(noexcept(form_decode_query(std::string_view{}, alloc, sink)));
    bool ok = false;
    {
        // An allocation here would throw through noexcept and terminate the test.
        fiber::test::ScopedAllocFailure no_allocations(0);
        ok = form_decode_query("a=%FF&%FE=%C3%A9&c", alloc, sink).has_value();
    }
    EXPECT_TRUE(ok);
    EXPECT_EQ(decoded_bytes, 10U); // a + U+FFFD, U+FFFD + e-acute, c
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
