#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fiber/regex/Regex.h>
#include <fiber/regex/Scratch.h>

namespace {

using fiber::regex::Encoding;
using fiber::regex::MatchStatus;
using fiber::regex::Options;
using fiber::regex::Regex;
using fiber::regex::Scratch;
using fiber::regex::Span;
using fiber::regex::WorkBudget;

Options bytes_mode() {
    Options options;
    options.encoding = Encoding::Bytes;
    return options;
}

Regex compiled(std::string_view pattern, const Options &options = {}) {
    auto regex = Regex::compile(pattern, options);
    EXPECT_TRUE(regex.has_value()) << pattern;
    return std::move(*regex);
}

bool matches(std::string_view pattern, std::string_view text, const Options &options = {}) {
    Scratch scratch;
    return compiled(pattern, options).test(text, scratch) == MatchStatus::Match;
}

// All non-overlapping matches, stepping past empty ones.
std::vector<std::string_view> find_all(const Regex &regex, std::string_view text) {
    Scratch scratch;
    std::vector<std::string_view> found;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        std::array<Span, 1> span;
        if (regex.find(text, pos, span, scratch) != MatchStatus::Match) {
            break;
        }
        found.push_back(text.substr(span[0].begin, span[0].end - span[0].begin));
        pos = span[0].end == span[0].begin ? span[0].end + 1 : span[0].end;
    }
    return found;
}

TEST(Regex, PrefersLeftmostFirst) {
    Scratch scratch;
    std::array<Span, 1> span;
    ASSERT_EQ(compiled("a|ab").find("ab", 0, span, scratch), MatchStatus::Match);
    EXPECT_EQ(span[0].end, 1U);
    ASSERT_EQ(compiled("ab|a").find("ab", 0, span, scratch), MatchStatus::Match);
    EXPECT_EQ(span[0].end, 2U);
    ASSERT_EQ(compiled("a*?").find("aaa", 0, span, scratch), MatchStatus::Match);
    EXPECT_EQ(span[0].end, 0U);
    ASSERT_EQ(compiled("a*").find("baaa", 0, span, scratch), MatchStatus::Match);
    EXPECT_EQ(span[0].end, 0U);
}

TEST(Regex, ReportsCaptures) {
    Scratch scratch;
    const Regex regex = compiled("(a)|(b)");
    std::array<Span, 3> spans;
    ASSERT_EQ(regex.find("xb", 0, spans, scratch), MatchStatus::Match);
    EXPECT_EQ(spans[0].begin, 1U);
    EXPECT_FALSE(spans[1].matched());
    EXPECT_TRUE(spans[2].matched());
    EXPECT_EQ(spans[2].begin, 1U);
    EXPECT_EQ(spans[2].end, 2U);
}

TEST(Regex, ComputesOnlyTheRequestedGroups) {
    Scratch scratch;
    const Regex regex = compiled("(a)(b)");

    std::array<Span, 2> fewer;
    ASSERT_EQ(regex.find("ab", 0, fewer, scratch), MatchStatus::Match);
    EXPECT_EQ(fewer[1].begin, 0U);

    std::array<Span, 5> more;
    ASSERT_EQ(regex.find("ab", 0, more, scratch), MatchStatus::Match);
    EXPECT_EQ(more[2].begin, 1U);
    EXPECT_FALSE(more[3].matched());
    EXPECT_FALSE(more[4].matched());

    EXPECT_EQ(regex.find("ab", 0, {}, scratch), MatchStatus::Match);
}

TEST(Regex, ClearsCapturesOnNoMatch) {
    Scratch scratch;
    const Regex regex = compiled("(a)");
    std::array<Span, 2> spans;
    ASSERT_EQ(regex.find("a", 0, spans, scratch), MatchStatus::Match);
    ASSERT_EQ(regex.find("b", 0, spans, scratch), MatchStatus::NoMatch);
    EXPECT_FALSE(spans[0].matched());
    EXPECT_FALSE(spans[1].matched());
}

TEST(Regex, RepeatedGroupsKeepTheLastIteration) {
    Scratch scratch;
    std::array<Span, 2> spans;
    ASSERT_EQ(compiled("(a|b){3}").find("abb", 0, spans, scratch), MatchStatus::Match);
    EXPECT_EQ(spans[1].begin, 2U);
    ASSERT_EQ(compiled("(?:(a)|b)*").find("ab", 0, spans, scratch), MatchStatus::Match);
    EXPECT_EQ(spans[1].begin, 0U);
}

TEST(Regex, StartKeepsEarlierTextAsContext) {
    Scratch scratch;
    std::array<Span, 1> span;

    // \b sees the 'a' before the start position.
    EXPECT_EQ(compiled("\\bb").find("ab b", 1, span, scratch), MatchStatus::Match);
    EXPECT_EQ(span[0].begin, 3U);

    // ^ and \A refer to the text, not to the start position.
    EXPECT_EQ(compiled("^b").find("ab", 1, span, scratch), MatchStatus::NoMatch);
    EXPECT_EQ(compiled("\\Ab").find("ab", 1, span, scratch), MatchStatus::NoMatch);
    Options multi_line;
    multi_line.multi_line = true;
    EXPECT_EQ(compiled("^b", multi_line).find("a\nb", 1, span, scratch), MatchStatus::Match);
    EXPECT_EQ(span[0].begin, 2U);

    EXPECT_EQ(compiled("a").find("a", 2, span, scratch), MatchStatus::NoMatch);
    EXPECT_EQ(compiled("").find("a", 1, span, scratch), MatchStatus::Match);
}

TEST(Regex, FindsEveryMatch) {
    using Found = std::vector<std::string_view>;
    EXPECT_EQ(find_all(compiled("a+"), "aa b aaa"), (Found{"aa", "aaa"}));
    EXPECT_EQ(find_all(compiled("x*"), "ab"), (Found{"", "", ""}));
    EXPECT_EQ(find_all(compiled("\\b"), "ab cd"), (Found{"", "", "", ""}));
}

TEST(Regex, HandlesEmptyInputs) {
    EXPECT_TRUE(matches("", ""));
    EXPECT_TRUE(matches("", "abc"));
    EXPECT_TRUE(matches("^$", ""));
    EXPECT_FALSE(matches("a", ""));

    Scratch scratch;
    EXPECT_EQ(compiled("a?").test(std::string_view{}, scratch), MatchStatus::Match);
}

TEST(Regex, EmptyClassNeverMatches) {
    EXPECT_FALSE(matches("[^\\x00-\\x{10FFFF}]", "a"));
    EXPECT_FALSE(matches("[^\\x00-\\x{ff}]", "a", bytes_mode()));
    EXPECT_TRUE(matches("a|[^\\x00-\\x{10FFFF}]", "a"));
    EXPECT_TRUE(matches("[^\\x00-\\x{10FFFF}]*", "a"));
}

TEST(Regex, AppliesOptions) {
    Options ignore_case;
    ignore_case.ignore_case = true;
    EXPECT_TRUE(matches("hello", "HeLLo", ignore_case));
    EXPECT_TRUE(matches("[a-c]+", "ABC", ignore_case));
    EXPECT_FALSE(matches("hello", "HeLLo"));

    Options dot_all;
    dot_all.dot_matches_newline = true;
    EXPECT_TRUE(matches("a.b", "a\nb", dot_all));
    EXPECT_FALSE(matches("a.b", "a\nb"));

    Options multi_line;
    multi_line.multi_line = true;
    EXPECT_TRUE(matches("^b$", "a\nb\nc", multi_line));
    EXPECT_FALSE(matches("^b$", "a\nb\nc"));
}

TEST(Regex, InlineFlagsOverrideOptions) {
    Options ignore_case;
    ignore_case.ignore_case = true;
    EXPECT_FALSE(matches("(?-i)hello", "HELLO", ignore_case));
    EXPECT_TRUE(matches("(?i)hel(?-i)lo", "HELlo"));
    EXPECT_FALSE(matches("(?i)hel(?-i)lo", "HELLO"));
    EXPECT_TRUE(matches("(?i:a)b", "Ab"));
    EXPECT_FALSE(matches("(?i:a)b", "AB"));
}

TEST(Regex, DollarOnlyMatchesAtTheVeryEnd) {
    // Unlike default PCRE, and the same as ModSecurity's DOLLAR_ENDONLY.
    EXPECT_FALSE(matches("a$", "a\n"));
    EXPECT_TRUE(matches("a\\n$", "a\n"));
}

// feature/regex.md section 4.3.
TEST(Regex, BytesAndUtf8DifferOnHighBytes) {
    // \x{c2} is U+00C2 in Utf8 mode, which UTF-8 encodes as C3 82.
    EXPECT_TRUE(matches("\\x{c2}", "\xc3\x82"));
    EXPECT_FALSE(matches("\\x{c2}", "\xc2"));
    EXPECT_TRUE(matches("\\x{c2}", "\xc2", bytes_mode()));
    EXPECT_FALSE(matches("^\\x{c2}$", "\xc3\x82", bytes_mode()));

    // What CRS writes for the UTF-8 encoding of an acute accent.
    EXPECT_TRUE(matches("\\x{c2}\\x{b4}", "\xc2\xb4", bytes_mode()));
    EXPECT_FALSE(matches("\\x{c2}\\x{b4}", "\xc2\xb4"));
}

TEST(Regex, OneInvalidByteBreaksUtf8MatchesButNotBytes) {
    // The evasion that makes Bytes mode mandatory for inspecting traffic.
    EXPECT_FALSE(matches("a.b", "a\xff"
                                "b"));
    EXPECT_FALSE(matches("a[^x]b", "a\xff"
                                   "b"));
    EXPECT_TRUE(matches("a.b",
                        "a\xff"
                        "b",
                        bytes_mode()));
    EXPECT_TRUE(matches("a[^x]b",
                        "a\xff"
                        "b",
                        bytes_mode()));
}

TEST(Regex, DotIsOneCharacterInEachEncoding) {
    EXPECT_TRUE(matches("^.$", "é"));
    EXPECT_FALSE(matches("^.$", "é", bytes_mode()));
    EXPECT_TRUE(matches("^..$", "é", bytes_mode()));
    EXPECT_TRUE(matches("^.$", "😀"));
    EXPECT_TRUE(matches("^[€-₿]$", "€"));
}

TEST(Regex, UsesAsciiCaseFoldingOnly) {
    EXPECT_FALSE(matches("(?i)é", "É"));
    EXPECT_FALSE(matches("(?i)k", "\xe2\x84\xaa")); // KELVIN SIGN
    EXPECT_FALSE(matches("(?i)\\x{e9}", "\xc9", bytes_mode()));
    EXPECT_TRUE(matches("(?i)k", "K"));
}

// Literals keep the fold flag whatever they are, so the fold logic of every engine
// and of the byte classes has to cope with ranges nowhere near the letters.
TEST(Regex, CaseFoldingLeavesNonLettersAlone) {
    for (const std::string_view pattern:
         {"(?i)\\n", "(?i)\\x01", "(?i)^\\x01$", "(?i)[\\x01]", "(?i)\\x{7f}", "(?i)1"}) {
        EXPECT_TRUE(Regex::compile(pattern).has_value()) << pattern;
    }
    EXPECT_TRUE(matches("(?i)^\\x01$", "\x01"));
    EXPECT_FALSE(matches("(?i)^\\x01$", "\x21"));
    EXPECT_TRUE(matches("(?i)^a\\nb$", "A\nB"));
    EXPECT_FALSE(matches("(?i)^@$", "`")); // one bit away, like a letter pair
}

TEST(Regex, ClassesAreAscii) {
    EXPECT_FALSE(matches("\\w", "é"));
    EXPECT_FALSE(matches("\\d", "٣"));
    EXPECT_FALSE(matches("\\s", "\x0b"));
    EXPECT_TRUE(matches("[[:space:]]", "\x0b"));
    EXPECT_TRUE(matches("\\W", "é"));
}

TEST(Regex, MatchesAcrossPrefixSkips) {
    Scratch scratch;
    const std::string text = std::string(10000, 'x') + "needle" + std::string(10000, 'x');
    std::array<Span, 1> span;
    ASSERT_EQ(compiled("needle").find(text, 0, span, scratch), MatchStatus::Match);
    EXPECT_EQ(span[0].begin, 10000U);
    ASSERT_EQ(compiled("(needle)").find(text, 0, span, scratch), MatchStatus::Match);
    EXPECT_EQ(span[0].begin, 10000U);
    EXPECT_EQ(compiled("needles").find(text, 0, span, scratch), MatchStatus::NoMatch);
    EXPECT_EQ(compiled("x$").find(text, 0, span, scratch), MatchStatus::Match);
}

// These take exponential time in a backtracking engine.
TEST(Regex, StaysLinearOnPathologicalInput) {
    Scratch scratch;
    const std::string text(100000, 'a');
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(compiled("(a+)+$").test(text, scratch), MatchStatus::Match);
    EXPECT_EQ(compiled("(a+)+b").test(text, scratch), MatchStatus::NoMatch);
    EXPECT_EQ(compiled("(a|a)*b").test(text, scratch), MatchStatus::NoMatch);
    EXPECT_EQ(compiled("(a|aa)*b").test(text, scratch), MatchStatus::NoMatch);
    EXPECT_EQ(compiled("(.*)(.*)(.*)(.*)(.*)b").test(text, scratch), MatchStatus::NoMatch);
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(30));
}

TEST(Regex, BudgetExhaustionIsNotNoMatch) {
    Scratch scratch;
    const std::string text(1000, 'a');
    const Regex regex = compiled("(a+)+b");

    WorkBudget tiny{10};
    EXPECT_EQ(regex.test(text, scratch, tiny), MatchStatus::BudgetExhausted);

    std::array<Span, 1> span;
    WorkBudget also_tiny{10};
    EXPECT_EQ(regex.find(text, 0, span, scratch, also_tiny), MatchStatus::BudgetExhausted);
    EXPECT_FALSE(span[0].matched());

    WorkBudget plenty;
    EXPECT_EQ(regex.test(text, scratch, plenty), MatchStatus::NoMatch);
}

TEST(Regex, BudgetIsSharedAcrossCalls) {
    Scratch scratch;
    const std::string text(100, 'a');
    const Regex regex = compiled("b");

    WorkBudget budget{1000};
    EXPECT_EQ(regex.test(text, scratch, budget), MatchStatus::NoMatch);
    const std::uint64_t after_one = budget.remaining;
    EXPECT_LT(after_one, 1000U);
    EXPECT_EQ(regex.test(text, scratch, budget), MatchStatus::NoMatch);
    EXPECT_LT(budget.remaining, after_one);
}

TEST(Regex, ScratchWorksAcrossPrograms) {
    Scratch scratch;
    const Regex small = compiled("a");
    const Regex large = compiled("(?:[a-z]{50}){10}(x)");
    const std::string text = std::string(500, 'q') + "x";
    std::array<Span, 2> spans;
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(small.test("a", scratch), MatchStatus::Match);
        EXPECT_EQ(large.find(text, 0, spans, scratch), MatchStatus::Match);
        EXPECT_EQ(spans[1].begin, 500U);
    }
}

TEST(Regex, CopiesShareTheProgram) {
    Scratch scratch;
    const Regex first = compiled("a(b)");
    const Regex copy = first; // NOLINT(performance-unnecessary-copy-initialization) copying is what is under test
    EXPECT_EQ(copy.capture_count(), 1U);
    EXPECT_EQ(copy.test("ab", scratch), MatchStatus::Match);
}

TEST(Regex, CompiledRegexIsSharedBetweenThreads) {
    const Regex regex = compiled("(\\d+)-(\\d+)");
    std::vector<std::thread> threads;
    std::array<int, 4> failures{};
    threads.reserve(failures.size());
    for (std::size_t t = 0; t < failures.size(); ++t) {
        threads.emplace_back([&regex, &failures, t] {
            Scratch scratch;
            std::array<Span, 3> spans;
            const std::string text = "id " + std::to_string(t) + "-42";
            for (int i = 0; i < 2000; ++i) {
                if (regex.find(text, 0, spans, scratch) != MatchStatus::Match || spans[2].end != text.size()) {
                    ++failures[t];
                }
            }
        });
    }
    for (std::thread &thread: threads) {
        thread.join();
    }
    EXPECT_EQ(failures, (std::array<int, 4>{}));
}

} // namespace
