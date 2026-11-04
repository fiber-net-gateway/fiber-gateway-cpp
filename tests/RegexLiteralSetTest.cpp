#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fiber/regex/LiteralSet.h>

namespace {

using fiber::regex::ErrorCode;
using fiber::regex::LiteralSet;
using fiber::regex::MatchSet;

LiteralSet build(const std::vector<std::string> &literals, bool ignore_case = false) {
    LiteralSet::Builder builder(ignore_case);
    for (const std::string &literal: literals) {
        EXPECT_TRUE(builder.add(literal).has_value()) << literal;
    }
    return std::move(builder).build();
}

std::vector<std::uint32_t> hits_of(const MatchSet &hits) {
    std::vector<std::uint32_t> ids;
    for (std::uint32_t i = 0; i < hits.size(); ++i) {
        if (hits.contains(i)) {
            ids.push_back(i);
        }
    }
    return ids;
}

std::vector<std::uint32_t> scan(const LiteralSet &set, std::string_view text) {
    MatchSet hits = set.make_match_set();
    set.scan(text, hits);
    return hits_of(hits);
}

using Ids = std::vector<std::uint32_t>;

// The example from the Aho-Corasick paper, where matches overlap and nest.
TEST(LiteralSet, FindsOverlappingLiterals) {
    const LiteralSet set = build({"he", "she", "his", "hers"});
    EXPECT_EQ(scan(set, "ushers"), (Ids{0, 1, 3}));
    EXPECT_EQ(scan(set, "this"), (Ids{2}));
    EXPECT_EQ(scan(set, "nothing"), (Ids{}));
    EXPECT_EQ(scan(set, ""), (Ids{}));
}

TEST(LiteralSet, FindsSuffixesThroughFailLinks) {
    const LiteralSet set = build({"abcd", "bc", "c"});
    EXPECT_EQ(scan(set, "abcx"), (Ids{1, 2}));
    EXPECT_EQ(scan(set, "abcd"), (Ids{0, 1, 2}));
}

TEST(LiteralSet, DuplicatesKeepTheirOwnIds) {
    const LiteralSet set = build({"same", "other", "same"});
    EXPECT_EQ(scan(set, "the same"), (Ids{0, 2}));
}

TEST(LiteralSet, FoldsAsciiCaseOnRequest) {
    const LiteralSet sensitive = build({"SeLeCt"});
    EXPECT_EQ(scan(sensitive, "select"), (Ids{}));
    EXPECT_EQ(scan(sensitive, "SeLeCt"), (Ids{0}));

    const LiteralSet insensitive = build({"SeLeCt"}, true);
    EXPECT_EQ(scan(insensitive, "SELECT"), (Ids{0}));
    EXPECT_EQ(scan(insensitive, "select"), (Ids{0}));

    const LiteralSet accented = build({"\xc3\xa9"}, true);
    EXPECT_EQ(scan(accented, "\xc3\x89"), (Ids{})); // É is not folded
}

TEST(LiteralSet, HandlesBinaryLiterals) {
    const std::string nul("a\0b", 3);
    const LiteralSet set = build({nul, "\xff\xfe"});
    EXPECT_EQ(scan(set, std::string("xa\0by", 5)), (Ids{0}));
    EXPECT_EQ(scan(set, "\xff\xfe"), (Ids{1}));
    EXPECT_EQ(scan(set, "ab"), (Ids{}));
}

TEST(LiteralSet, RejectsEmptyLiterals) {
    LiteralSet::Builder builder;
    auto id = builder.add("");
    ASSERT_FALSE(id.has_value());
    EXPECT_EQ(id.error().code, ErrorCode::Syntax);
    EXPECT_EQ(builder.add("next"), 0U);
}

TEST(LiteralSet, EmptySetFindsNothing) {
    const LiteralSet set = build({});
    EXPECT_EQ(set.size(), 0U);
    EXPECT_FALSE(set.contains_any("anything"));
    EXPECT_EQ(scan(set, "anything"), (Ids{}));
}

TEST(LiteralSet, ContainsAnyAgreesWithScan) {
    const LiteralSet set = build({"needle", "pin"});
    EXPECT_TRUE(set.contains_any("a needle here"));
    EXPECT_TRUE(set.contains_any("spinning"));
    EXPECT_FALSE(set.contains_any("haystack"));
    EXPECT_FALSE(set.contains_any(""));
}

TEST(LiteralSet, MatchesSpanChunks) {
    const LiteralSet set = build({"hello world", "low", "xyz"});
    MatchSet hits = set.make_match_set();
    LiteralSet::Cursor cursor;
    set.feed("hel", cursor, hits);
    EXPECT_FALSE(hits.any());
    set.feed("lo wo", cursor, hits);
    EXPECT_FALSE(hits.any());
    set.feed("rld", cursor, hits);
    EXPECT_EQ(hits_of(hits), (Ids{0}));
}

TEST(LiteralSet, BusyNodesUseDenseRows) {
    // More than enough edges out of one node for it to switch representation.
    std::vector<std::string> literals;
    for (char c = 'a'; c <= 'z'; ++c) {
        literals.push_back(std::string("ab") + c);
    }
    const LiteralSet set = build(literals);
    EXPECT_EQ(scan(set, "abq"), (Ids{16}));
    EXPECT_EQ(scan(set, "xxabzz"), (Ids{25}));
    EXPECT_EQ(scan(set, "ab"), (Ids{}));
    EXPECT_EQ(scan(set, "aab"), (Ids{}));
}

TEST(LiteralSet, AgreesWithNaiveSearch) {
    std::mt19937 random(31337);
    const auto random_string = [&random](std::size_t max_length, std::size_t alphabet) {
        std::string text(1 + (random() % max_length), '\0');
        for (char &c: text) {
            c = static_cast<char>('a' + (random() % alphabet));
        }
        return text;
    };

    for (int round = 0; round < 2000; ++round) {
        std::vector<std::string> literals;
        const std::size_t count = 1 + (random() % 20);
        literals.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            literals.push_back(random_string(6, 3));
        }
        const LiteralSet set = build(literals);

        const std::string text = random_string(40, 3);
        Ids expected;
        for (std::uint32_t i = 0; i < literals.size(); ++i) {
            if (text.contains(literals[i])) {
                expected.push_back(i);
            }
        }
        ASSERT_EQ(scan(set, text), expected) << text;
        ASSERT_EQ(set.contains_any(text), !expected.empty()) << text;

        // Any way of cutting the text up must give the same answer.
        MatchSet hits = set.make_match_set();
        LiteralSet::Cursor cursor;
        std::size_t pos = 0;
        while (pos < text.size()) {
            const std::size_t length = 1 + (random() % 5);
            set.feed(std::string_view(text).substr(pos, length), cursor, hits);
            pos += length;
        }
        ASSERT_EQ(hits_of(hits), expected) << text;
    }
}

} // namespace
