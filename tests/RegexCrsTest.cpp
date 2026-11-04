// OWASP CRS is the acceptance set for the syntax fiber::regex must handle: see
// feature/regex.md section 2.
#include <gtest/gtest.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fiber/regex/LiteralSet.h>
#include <fiber/regex/Regex.h>
#include <fiber/regex/RegexSet.h>
#include <fiber/regex/Scratch.h>

namespace {

using fiber::regex::Encoding;
using fiber::regex::LiteralSet;
using fiber::regex::MatchSet;
using fiber::regex::MatchStatus;
using fiber::regex::Options;
using fiber::regex::Regex;
using fiber::regex::RegexSet;
using fiber::regex::Scratch;

std::filesystem::path fixture_dir() {
    return std::filesystem::path(__FILE__).parent_path() / "support" / "third_party" / "crs_bd1c42a";
}

std::string from_hex(std::string_view hex) {
    std::string out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        unsigned value = 0;
        std::from_chars(hex.data() + i, hex.data() + i + 2, value, 16);
        out.push_back(static_cast<char>(value));
    }
    return out;
}

struct Rule {
    std::string location;
    std::string pattern;
};

std::vector<Rule> load_rules() {
    const auto path = fixture_dir() / "rx_patterns.txt";
    std::ifstream file(path);
    EXPECT_TRUE(file.is_open()) << path;
    std::vector<Rule> rules;
    std::string line;
    while (std::getline(file, line)) {
        const std::size_t tab = line.find('\t');
        rules.push_back(Rule{.location = line.substr(0, tab), .pattern = line.substr(tab + 1)});
    }
    return rules;
}

// How ModSecurity runs @rx: bytes, with DOTALL. Its DOLLAR_ENDONLY is the default.
Options waf_options() {
    Options options;
    options.encoding = Encoding::Bytes;
    options.dot_matches_newline = true;
    return options;
}

TEST(RegexCrs, CompilesEveryRule) {
    const std::vector<Rule> rules = load_rules();
    ASSERT_EQ(rules.size(), 320U);
    for (const Rule &rule: rules) {
        auto regex = Regex::compile(rule.pattern, waf_options());
        EXPECT_TRUE(regex.has_value()) << rule.location;
    }
}

TEST(RegexCrs, DetectsRepresentativeAttacks) {
    const std::vector<Rule> rules = load_rules();
    std::vector<Regex> regexes;
    regexes.reserve(rules.size());
    for (const Rule &rule: rules) {
        auto regex = Regex::compile(rule.pattern, waf_options());
        ASSERT_TRUE(regex.has_value()) << rule.location;
        regexes.push_back(std::move(*regex));
    }

    Scratch scratch;
    const auto hits = [&](std::string_view payload) {
        return std::count_if(regexes.begin(), regexes.end(),
                             [&](const Regex &regex) { return regex.test(payload, scratch) == MatchStatus::Match; });
    };
    EXPECT_GT(hits("1' OR '1'='1' UNION SELECT password FROM users--"), 0);
    EXPECT_GT(hits("<script>alert(document.cookie)</script>"), 0);
    EXPECT_GT(hits("../../../../etc/passwd"), 0);
    EXPECT_GT(hits("() { :; }; /bin/bash -c 'cat /etc/passwd'"), 0);
}

TEST(RegexCrs, RuleSetAgreesWithRulesTakenOneByOne) {
    const std::vector<Rule> rules = load_rules();
    RegexSet::Builder builder(waf_options());
    std::vector<Regex> regexes;
    for (const Rule &rule: rules) {
        ASSERT_TRUE(builder.add(rule.pattern).has_value()) << rule.location;
        regexes.push_back(std::move(*Regex::compile(rule.pattern, waf_options())));
    }
    auto set = std::move(builder).build();
    ASSERT_TRUE(set.has_value());

    Scratch scratch;
    for (const std::string_view payload:
         {"1' OR '1'='1' UNION SELECT password FROM users--", "<script>alert(document.cookie)</script>",
          "../../../../etc/passwd", "GET /index.html HTTP/1.1", "plain text with nothing to see"}) {
        MatchSet hits = set->make_match_set();
        (void) set->scan(payload, hits, scratch);
        for (std::uint32_t i = 0; i < regexes.size(); ++i) {
            EXPECT_EQ(hits.contains(i), regexes[i].test(payload, scratch) == MatchStatus::Match) << rules[i].location;
        }
    }
}

// Every rule against real attack payloads, compared with what Go's regexp says.
TEST(RegexCrs, MatchesLikeTheOracleOnAttackPayloads) {
    const std::vector<Rule> rules = load_rules();
    std::vector<Regex> regexes;
    regexes.reserve(rules.size());
    for (const Rule &rule: rules) {
        regexes.push_back(std::move(*Regex::compile(rule.pattern, waf_options())));
    }

    std::ifstream file(fixture_dir() / "payloads.txt");
    ASSERT_TRUE(file.is_open());

    Scratch scratch;
    std::size_t hits = 0;
    std::size_t payloads = 0;
    std::string line;
    while (std::getline(file, line)) {
        const std::size_t tab = line.find('\t');
        const std::string payload = from_hex(std::string_view(line).substr(0, tab));
        std::vector<bool> expected(rules.size(), false);
        std::string_view ids = std::string_view(line).substr(tab + 1);
        while (!ids.empty()) {
            const std::size_t comma = ids.find(',');
            const std::string_view id = ids.substr(0, comma);
            std::size_t index = 0;
            std::from_chars(id.data(), id.data() + id.size(), index);
            expected[index] = true;
            ids.remove_prefix(comma == std::string_view::npos ? ids.size() : comma + 1);
        }

        // Go folds the Latin-1 letters too, which says nothing about this engine.
        const bool high_bytes = std::any_of(payload.begin(), payload.end(),
                                            [](char c) { return (static_cast<unsigned char>(c) & 0x80U) != 0; });
        for (std::size_t i = 0; i < rules.size(); ++i) {
            if (high_bytes && rules[i].pattern.contains("(?i")) {
                continue;
            }
            const bool matched = regexes[i].test(payload, scratch) == MatchStatus::Match;
            EXPECT_EQ(matched, expected[i]) << rules[i].location << " on " << payload;
            hits += matched ? 1 : 0;
        }
        ++payloads;
    }
    EXPECT_EQ(payloads, 1500U);
    EXPECT_GT(hits, 10000U);
}

// The word lists behind @pmFromFile, which matches case-insensitively.
TEST(RegexCrs, LoadsEveryWordList) {
    std::size_t files = 0;
    for (const auto &entry: std::filesystem::directory_iterator(fixture_dir() / "data")) {
        std::ifstream file(entry.path());
        ASSERT_TRUE(file.is_open()) << entry.path();

        LiteralSet::Builder builder(true);
        std::vector<std::string> words;
        std::string line;
        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#') {
                continue;
            }
            ASSERT_TRUE(builder.add(line).has_value()) << entry.path();
            words.push_back(line);
        }
        const LiteralSet set = std::move(builder).build();
        ++files;

        // A dense table for the largest list would take 44 MiB.
        EXPECT_LT(set.memory_bytes(), std::size_t{4} << 20U) << entry.path();

        for (const std::string &word: words) {
            EXPECT_TRUE(set.contains_any("xx" + word + "yy")) << entry.path() << ": " << word;
        }
    }
    EXPECT_EQ(files, 20U);
}

} // namespace
