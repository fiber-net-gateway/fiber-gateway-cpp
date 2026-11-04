#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fiber/regex/Regex.h>
#include <fiber/regex/RegexSet.h>
#include <fiber/regex/Scratch.h>

namespace {

using fiber::regex::Encoding;
using fiber::regex::ErrorCode;
using fiber::regex::MatchSet;
using fiber::regex::MatchStatus;
using fiber::regex::Options;
using fiber::regex::Regex;
using fiber::regex::RegexSet;
using fiber::regex::Scanner;
using fiber::regex::Scratch;
using fiber::regex::WorkBudget;

RegexSet build_set(const std::vector<std::string> &patterns, const Options &options = {}) {
    RegexSet::Builder builder(options);
    for (const std::string &pattern: patterns) {
        EXPECT_TRUE(builder.add(pattern).has_value()) << pattern;
    }
    auto set = std::move(builder).build();
    EXPECT_TRUE(set.has_value());
    return std::move(*set);
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

std::vector<std::uint32_t> scan(const RegexSet &set, std::string_view text, Scratch &scratch) {
    MatchSet hits = set.make_match_set();
    const MatchStatus status = set.scan(text, hits, scratch);
    EXPECT_EQ(status == MatchStatus::Match, hits.any()) << text;
    return hits_of(hits);
}

// What each pattern reports on its own, which is what the set must agree with.
std::vector<std::uint32_t> one_by_one(const std::vector<std::string> &patterns, std::string_view text, Scratch &scratch,
                                      const Options &options = {}) {
    std::vector<std::uint32_t> ids;
    for (std::uint32_t i = 0; i < patterns.size(); ++i) {
        auto regex = Regex::compile(patterns[i], options);
        EXPECT_TRUE(regex.has_value()) << patterns[i];
        if (regex->test(text, scratch) == MatchStatus::Match) {
            ids.push_back(i);
        }
    }
    return ids;
}

using Ids = std::vector<std::uint32_t>;

TEST(RegexSet, ReportsEveryMatchingPattern) {
    const RegexSet set = build_set({"foo", "bar", "foo.*bar", "^baz$", "qux"});
    Scratch scratch;
    EXPECT_EQ(scan(set, "foo", scratch), (Ids{0}));
    EXPECT_EQ(scan(set, "bar foo", scratch), (Ids{0, 1}));
    EXPECT_EQ(scan(set, "foo bar", scratch), (Ids{0, 1, 2}));
    EXPECT_EQ(scan(set, "baz", scratch), (Ids{3}));
    EXPECT_EQ(scan(set, " baz", scratch), (Ids{}));
    EXPECT_EQ(scan(set, "", scratch), (Ids{}));
}

TEST(RegexSet, KeepsIdsAcrossSorting) {
    // Added out of order: the builder sorts patterns, and ids must survive that.
    const RegexSet set = build_set({"zzz", "aaa", "mmm"});
    Scratch scratch;
    EXPECT_EQ(scan(set, "aaa", scratch), (Ids{1}));
    EXPECT_EQ(scan(set, "zzz", scratch), (Ids{0}));
    EXPECT_EQ(scan(set, "mmm", scratch), (Ids{2}));
}

TEST(RegexSet, HandlesEmptyAndAnchoredPatterns) {
    const RegexSet set = build_set({"", "^$", "^", "$", "\\b", "a$"});
    Scratch scratch;
    EXPECT_EQ(scan(set, "", scratch), (Ids{0, 1, 2, 3}));
    EXPECT_EQ(scan(set, "a", scratch), (Ids{0, 2, 3, 4, 5}));
    EXPECT_EQ(scan(set, "ab", scratch), (Ids{0, 2, 3, 4}));
}

TEST(RegexSet, EmptySetMatchesNothing) {
    const RegexSet set = build_set({});
    Scratch scratch;
    EXPECT_EQ(set.size(), 0U);
    EXPECT_EQ(scan(set, "anything", scratch), (Ids{}));
}

TEST(RegexSet, PatternsThatCannotMatchAreAllowed) {
    const RegexSet set = build_set({"[^\\x00-\\x{10FFFF}]", "a"});
    Scratch scratch;
    EXPECT_EQ(scan(set, "a", scratch), (Ids{1}));
}

TEST(RegexSet, ReportsBadPatternsAtAdd) {
    RegexSet::Builder builder{Options{}};
    ASSERT_TRUE(builder.add("ok").has_value());
    auto bad = builder.add("(");
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().code, ErrorCode::Syntax);
    EXPECT_EQ(builder.add("(?=a)").error().code, ErrorCode::UnsupportedFeature);
    // Rejected patterns take no id.
    EXPECT_EQ(builder.add("next"), 1U);
}

TEST(RegexSet, IgnoresCapturingGroups) {
    const RegexSet set = build_set({"(a)(b)", "(?P<name>c)", "(?P<name>d)"});
    Scratch scratch;
    EXPECT_EQ(scan(set, "ab d", scratch), (Ids{0, 2}));
}

TEST(RegexSet, BudgetExhaustionIsNotNoMatch) {
    const RegexSet set = build_set({"a+b", "c+d"});
    Scratch scratch;
    const std::string text(1000, 'a');
    MatchSet hits = set.make_match_set();
    WorkBudget tiny{10};
    EXPECT_EQ(set.scan(text, hits, scratch, tiny), MatchStatus::BudgetExhausted);
}

TEST(RegexSet, LimitsProgramSize) {
    Options options;
    options.max_insts = 16; // a set gets 16 times that
    RegexSet::Builder builder(options);
    for (int i = 0; i < 40; ++i) {
        ASSERT_TRUE(builder.add("pattern-number-" + std::to_string(i) + "[a-z]{3}").has_value());
    }
    auto set = std::move(builder).build();
    ASSERT_FALSE(set.has_value());
    EXPECT_EQ(set.error().code, ErrorCode::ProgramTooLarge);
}

// ---- Scanner ----

TEST(RegexScanner, MatchesAcrossChunks) {
    const RegexSet set = build_set({"hello world", "wor", "^hello", "world$", "xyz"});
    Scratch scratch;
    WorkBudget budget;
    Scanner scanner(set);
    EXPECT_EQ(scanner.feed("hel", scratch, budget), MatchStatus::NoMatch);
    EXPECT_EQ(scanner.feed("lo wo", scratch, budget), MatchStatus::Match);
    EXPECT_EQ(scanner.feed("rld", scratch, budget), MatchStatus::Match);
    EXPECT_EQ(scanner.finish(scratch, budget), MatchStatus::Match);
    EXPECT_EQ(hits_of(scanner.hits()), (Ids{0, 1, 2, 3}));
}

TEST(RegexScanner, EndOfTextIsOnlyKnownAtFinish) {
    const RegexSet set = build_set({"a$", "a\\z", "a\\b"});
    Scratch scratch;
    WorkBudget budget;
    Scanner scanner(set);
    EXPECT_EQ(scanner.feed("a", scratch, budget), MatchStatus::NoMatch);
    EXPECT_FALSE(scanner.hits().any());
    EXPECT_EQ(scanner.finish(scratch, budget), MatchStatus::Match);
    EXPECT_EQ(hits_of(scanner.hits()), (Ids{0, 1, 2}));
}

TEST(RegexScanner, WordBoundariesWorkAcrossChunks) {
    const RegexSet set = build_set({"\\bcat\\b"});
    Scratch scratch;
    WorkBudget budget;

    Scanner across(set);
    (void) across.feed("a c", scratch, budget);
    (void) across.feed("at b", scratch, budget);
    (void) across.finish(scratch, budget);
    EXPECT_TRUE(across.hits().contains(0));

    // The word character before the chunk boundary has to be remembered.
    Scanner glued(set);
    (void) glued.feed("xc", scratch, budget);
    (void) glued.feed("at", scratch, budget);
    (void) glued.finish(scratch, budget);
    EXPECT_FALSE(glued.hits().any());
}

TEST(RegexScanner, EmptyStreams) {
    const RegexSet set = build_set({"", "^$", "a"});
    Scratch scratch;
    WorkBudget budget;

    Scanner no_feed(set);
    EXPECT_EQ(no_feed.finish(scratch, budget), MatchStatus::Match);
    EXPECT_EQ(hits_of(no_feed.hits()), (Ids{0, 1}));

    Scanner empty_chunks(set);
    (void) empty_chunks.feed("", scratch, budget);
    (void) empty_chunks.feed("", scratch, budget);
    (void) empty_chunks.finish(scratch, budget);
    EXPECT_EQ(hits_of(empty_chunks.hits()), (Ids{0, 1}));
}

TEST(RegexScanner, ReportsExhaustion) {
    const RegexSet set = build_set({"abc"});
    Scratch scratch;
    Scanner scanner(set);
    WorkBudget tiny{2};
    EXPECT_EQ(scanner.feed("xxxxxxxx", scratch, tiny), MatchStatus::BudgetExhausted);
}

TEST(RegexScanner, OutlivesTheSetItCameFrom) {
    Scratch scratch;
    WorkBudget budget;
    std::optional<Scanner> scanner;
    {
        const RegexSet set = build_set({"abc"});
        scanner.emplace(set);
    }
    (void) scanner->feed("abc", scratch, budget);
    EXPECT_EQ(scanner->finish(scratch, budget), MatchStatus::Match);
}

// Two streams sharing one worker's cache. Each feed() can reset the cache under the
// other stream, whose state has to survive that.
TEST(RegexScanner, InterleavedStreamsSurviveCacheResets) {
    const std::vector<std::string> patterns = {"[a-c]*abc[a-c]*x", "(?:a|b)*bab", "c{3,}a", "\\babc\\b", "b.c.a"};
    const RegexSet set = build_set(patterns);

    std::mt19937 random(12345);
    std::uniform_int_distribution<int> letter(0, 3);
    const auto random_text = [&](std::size_t length) {
        std::string text;
        for (std::size_t i = 0; i < length; ++i) {
            text.push_back("abcx"[letter(random)]);
        }
        return text;
    };

    for (const std::size_t budget_bytes:
         {std::size_t{0}, std::size_t{1500}, std::size_t{6000}, std::size_t{1} << 20U}) {
        Scratch scratch(budget_bytes);
        Scratch reference;
        for (int round = 0; round < 40; ++round) {
            const std::string first = random_text(300);
            const std::string second = random_text(300);
            WorkBudget budget;
            Scanner a(set);
            Scanner b(set);
            for (std::size_t pos = 0; pos < first.size(); pos += 7) {
                (void) a.feed(std::string_view(first).substr(pos, 7), scratch, budget);
                (void) b.feed(std::string_view(second).substr(pos, 7), scratch, budget);
            }
            (void) a.finish(scratch, budget);
            (void) b.finish(scratch, budget);
            EXPECT_EQ(hits_of(a.hits()), one_by_one(patterns, first, reference)) << budget_bytes;
            EXPECT_EQ(hits_of(b.hits()), one_by_one(patterns, second, reference)) << budget_bytes;
        }
    }
}

// ---- Randomized agreement ----

std::string random_pattern(std::mt19937 &random, int depth) {
    static const std::array<std::string_view, 16> atoms = {"a", "b", "c",   ".",   "[ab]", "[^a]", "\\w",   "\\d",
                                                           "^", "$", "\\b", "\\B", "",     "\\n",  "(a|b)", "(?i:B)"};
    const auto pick = [&random](int count) { return static_cast<int>(random() % static_cast<unsigned>(count)); };
    if (depth == 0 || pick(4) == 0) {
        return std::string(atoms[static_cast<std::size_t>(pick(atoms.size()))]);
    }
    const std::string left = random_pattern(random, depth - 1);
    switch (pick(8)) {
        case 0:
            return left + "*";
        case 1:
            return left + "+";
        case 2:
            return left + "?";
        case 3:
            return "(" + left + ")";
        case 4:
            return "(?:" + left + "){1,2}";
        case 5:
            return left + "|" + random_pattern(random, depth - 1);
        default:
            return left + random_pattern(random, depth - 1);
    }
}

std::string random_text(std::mt19937 &random) {
    static constexpr std::string_view kAlphabet = "abcAB1_ \n";
    std::string text(random() % 12, '\0');
    for (char &c: text) {
        c = kAlphabet[random() % kAlphabet.size()];
    }
    return text;
}

TEST(RegexSet, AgreesWithIndividualRegexes) {
    std::mt19937 random(2024);
    for (const Encoding encoding: {Encoding::Utf8, Encoding::Bytes}) {
        Options options;
        options.encoding = encoding;
        Scratch scratch;
        Scratch reference;
        for (int round = 0; round < 300; ++round) {
            std::vector<std::string> patterns;
            RegexSet::Builder builder(options);
            while (patterns.size() < 6) {
                std::string pattern = random_pattern(random, 3);
                // Repeating an empty-width operator is a syntax error in places.
                if (Regex::compile(pattern, options).has_value() && builder.add(pattern).has_value()) {
                    patterns.push_back(std::move(pattern));
                }
            }
            auto set = std::move(builder).build();
            ASSERT_TRUE(set.has_value());
            for (int i = 0; i < 6; ++i) {
                const std::string text = random_text(random);
                const Ids expected = one_by_one(patterns, text, reference, options);
                ASSERT_EQ(scan(*set, text, scratch), expected) << "text [" << text << "]";

                // Any way of cutting the text up must give the same answer.
                WorkBudget budget;
                Scanner scanner(*set);
                std::size_t pos = 0;
                while (pos < text.size()) {
                    const std::size_t length = 1 + (random() % 4);
                    (void) scanner.feed(std::string_view(text).substr(pos, length), scratch, budget);
                    pos += length;
                }
                (void) scanner.finish(scratch, budget);
                ASSERT_EQ(hits_of(scanner.hits()), expected) << "chunked text [" << text << "]";
            }
        }
    }
}

} // namespace
