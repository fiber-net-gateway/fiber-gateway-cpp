#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>

#include <fiber/regex/Regex.h>

namespace {

using fiber::regex::Encoding;
using fiber::regex::Error;
using fiber::regex::ErrorCode;
using fiber::regex::Options;
using fiber::regex::Regex;

ErrorCode error_of(std::string_view pattern, const Options &options = {}) {
    auto compiled = Regex::compile(pattern, options);
    EXPECT_FALSE(compiled.has_value()) << pattern;
    return compiled ? ErrorCode::Syntax : compiled.error().code;
}

std::string repeat(std::string_view text, std::size_t count) {
    std::string out;
    out.reserve(text.size() * count);
    for (std::size_t i = 0; i < count; ++i) {
        out += text;
    }
    return out;
}

// The invalid expressions of RE2's parse_test.cc.
TEST(RegexParser, RejectsMalformedPatterns) {
    for (const std::string_view pattern: {
                 "(",
                 ")",
                 "(a",
                 "(a|b|",
                 "(a|b",
                 "[a-z",
                 "([a-z)",
                 "x{1001}",
                 "\xff",
                 "[\xff]",
                 "[\\\xff]",
                 "\\\xff",
                 "(?P<name>a",
                 "(?P<name>",
                 "(?P<name",
                 "(?P<x y>a)",
                 "(?P<>a)",
                 "(?<name>a",
                 "(?<name>",
                 "(?<name",
                 "(?<x y>a)",
                 "(?<>a)",
                 "[a-Z]",
                 "(?i)[a-Z]",
                 "a{100000}",
                 "a{100000,}",
                 "((((((((((x{2}){2}){2}){2}){2}){2}){2}){2}){2}){2})",
                 "(((x{7}){11}){13})",
                 "\\Q\\E*",
         }) {
        EXPECT_FALSE(Regex::compile(pattern).has_value()) << pattern;
    }
}

TEST(RegexParser, RejectsMoreSyntaxErrors) {
    for (const std::string_view pattern:
         {"*",   "+",    "?",    "a**",    "a{2}*", "(*)",  "a|*",  "\\",    "\\q",         "[[:foo:]]",       "a{2,1}",
          "(?i", "(?z)", "(?-)", "(?--i)", "\\x",   "\\xg", "\\x{", "\\x{}", "\\x{110000}", "(?P<n>a)(?P<n>b)"}) {
        EXPECT_EQ(error_of(pattern), ErrorCode::Syntax) << pattern;
    }
}

TEST(RegexParser, NamesFeaturesItWillNotImplement) {
    for (const std::string_view pattern:
         {"\\1",   "(a)\\1", "\\9",     "\\g1",  "\\k<n>", "(?=a)",  "(?!a)",      "(?<=a)", "(?<!a)", "(?>a)",
          "a*+",   "a++",    "a?+",     "a{2}+", "\\K",    "\\C",    "\\p{Greek}", "\\pL",   "\\PL",   "[\\p{L}]",
          "(?|a)", "(?#c)",  "(?(1)a)", "(?R)",  "(?1)",   "(?P=n)", "(?P>n)",     "\\Z",    "\\G",    "\\R"}) {
        EXPECT_EQ(error_of(pattern), ErrorCode::UnsupportedFeature) << pattern;
    }
}

TEST(RegexParser, ReportsTheOffsetOfTheProblem) {
    const auto offset_of = [](std::string_view pattern) {
        auto compiled = Regex::compile(pattern);
        EXPECT_FALSE(compiled.has_value());
        return compiled ? std::uint32_t{0} : compiled.error().offset;
    };
    EXPECT_EQ(offset_of("abc(?=d)"), 3U);
    EXPECT_EQ(offset_of("ab\\1"), 2U);
    EXPECT_EQ(offset_of("abc[z-a]"), 4U);
    EXPECT_EQ(offset_of("ab{2,1}"), 2U);
    EXPECT_EQ(offset_of("ab)"), 2U);
}

TEST(RegexParser, AcceptsWhatLooksLikeAnError) {
    for (const std::string_view pattern: {"",
                                          "a{,2}",
                                          "a{",
                                          "a{1",
                                          "a{1,",
                                          "{",
                                          "}",
                                          "]",
                                          "[]a]",
                                          "[^]a]",
                                          "[a-]",
                                          "[-a]",
                                          "a|",
                                          "|a",
                                          "|",
                                          "()",
                                          "(|a)",
                                          "(?i)",
                                          "(?i:)",
                                          "(?:)",
                                          "\\Qa*\\E",
                                          "\\Q",
                                          "[[:alpha:][:^digit:]]",
                                          "\\_",
                                          "\\x{10FFFF}",
                                          "\\0",
                                          "\\07",
                                          "\\012",
                                          "(?P<n_1>a)",
                                          "(?<n>a)",
                                          "(?U)a+",
                                          "(?-i)a",
                                          "(?i-s)a",
                                          "(?m)^$",
                                          "a*?",
                                          "a+?",
                                          "a??",
                                          "a{1,2}?",
                                          R"(\d\D\s\S\w\W)"}) {
        EXPECT_TRUE(Regex::compile(pattern).has_value()) << pattern;
    }
}

TEST(RegexParser, LimitsPatternLength) {
    const std::string longest(std::size_t{64} * 1024U, 'a');
    Options options;
    options.max_insts = 1U << 20U;
    EXPECT_TRUE(Regex::compile(longest, options).has_value());
    EXPECT_EQ(error_of(longest + "a", options), ErrorCode::PatternTooLong);
}

TEST(RegexParser, LimitsRepeatCounts) {
    EXPECT_TRUE(Regex::compile("a{1000}").has_value());
    EXPECT_EQ(error_of("a{1001}"), ErrorCode::RepeatTooLarge);
    EXPECT_EQ(error_of("a{1,1001}"), ErrorCode::RepeatTooLarge);
    // The limit covers the product of nested repeats, which is what expands.
    EXPECT_TRUE(Regex::compile("(?:a{30}){30}").has_value());
    EXPECT_EQ(error_of("(?:a{100}){100}"), ErrorCode::RepeatTooLarge);
    EXPECT_EQ(error_of("(a{1000}){1000}"), ErrorCode::RepeatTooLarge);
}

TEST(RegexParser, LimitsNesting) {
    const auto nested = [](std::size_t depth) { return repeat("(", depth) + "a" + repeat(")", depth); };
    EXPECT_TRUE(Regex::compile(nested(999)).has_value());
    EXPECT_EQ(error_of(nested(1000)), ErrorCode::NestingTooDeep);
    EXPECT_EQ(error_of(nested(30000)), ErrorCode::NestingTooDeep);

    // Non-capturing groups leave no node behind, so they do not nest at all.
    EXPECT_TRUE(Regex::compile(repeat("(?:", 10000) + "a" + repeat(")", 10000)).has_value());
}

TEST(RegexParser, LimitsProgramSize) {
    Options tiny;
    tiny.max_insts = 8;
    EXPECT_TRUE(Regex::compile("abc", tiny).has_value());
    EXPECT_EQ(error_of("abcdefghij", tiny), ErrorCode::ProgramTooLarge);

    // Each repeat is within the limit, but together they exceed max_insts.
    EXPECT_EQ(error_of(repeat("[a-z]{1000}", 100)), ErrorCode::ProgramTooLarge);
}

TEST(RegexParser, ValidatesUtf8OnlyInUtf8Mode) {
    EXPECT_EQ(error_of("\xff"), ErrorCode::InvalidUtf8);
    EXPECT_EQ(error_of("\xc0\x80"), ErrorCode::InvalidUtf8); // overlong
    EXPECT_EQ(error_of("\xed\xa0\x80"), ErrorCode::InvalidUtf8); // surrogate
    EXPECT_EQ(error_of("\xf4\x90\x80\x80"), ErrorCode::InvalidUtf8); // above U+10FFFF

    Options bytes;
    bytes.encoding = Encoding::Bytes;
    EXPECT_TRUE(Regex::compile("\xff", bytes).has_value());
    EXPECT_TRUE(Regex::compile("\xc0\x80", bytes).has_value());
}

TEST(RegexParser, BytesModeOnlyExpressesBytes) {
    Options bytes;
    bytes.encoding = Encoding::Bytes;
    EXPECT_TRUE(Regex::compile("\\x{ff}", bytes).has_value());
    EXPECT_EQ(error_of("\\x{100}", bytes), ErrorCode::Syntax);
    EXPECT_EQ(error_of("[\\x{100}]", bytes), ErrorCode::Syntax);
    EXPECT_EQ(error_of("\\400", bytes), ErrorCode::Syntax);
    EXPECT_TRUE(Regex::compile("\\x{100}").has_value());
    EXPECT_TRUE(Regex::compile("\\400").has_value());
}

TEST(RegexParser, CountsAndNamesCaptures) {
    auto regex = Regex::compile("(a)(?:b)(?P<second>c)(?<third>d)");
    ASSERT_TRUE(regex.has_value());
    EXPECT_EQ(regex->capture_count(), 3U);
    EXPECT_EQ(regex->capture_index("second"), 2U);
    EXPECT_EQ(regex->capture_index("third"), 3U);
    EXPECT_EQ(regex->capture_index("missing"), std::nullopt);
}

} // namespace
