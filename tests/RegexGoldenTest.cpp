// Checks fiber::regex against expected results generated from Go's regexp package
// by scripts/regex/gen_golden.go. The inputs are RE2's search_test.cc table, run
// under RE2's flag variants, and randomly generated patterns.
#include <gtest/gtest.h>

#include <charconv>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fiber/regex/Regex.h>
#include <fiber/regex/Scratch.h>

#include "regex/ScratchState.h"

namespace {

using fiber::regex::Encoding;
using fiber::regex::MatchStatus;
using fiber::regex::Options;
using fiber::regex::Regex;
using fiber::regex::Scratch;
using fiber::regex::Span;
using fiber::regex::detail::Engine;
using fiber::regex::detail::ScratchAccess;

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

std::vector<std::string_view> split(std::string_view line, char separator) {
    std::vector<std::string_view> fields;
    std::size_t begin = 0;
    for (;;) {
        const std::size_t end = line.find(separator, begin);
        if (end == std::string_view::npos) {
            fields.push_back(line.substr(begin));
            return fields;
        }
        fields.push_back(line.substr(begin, end - begin));
        begin = end + 1;
    }
}

Span parse_span(std::string_view field) {
    if (field == "x") {
        return Span{};
    }
    const std::size_t colon = field.find(':');
    Span span;
    const std::string_view begin = field.substr(0, colon);
    std::from_chars(begin.data(), begin.data() + begin.size(), span.begin);
    std::from_chars(field.data() + colon + 1, field.data() + field.size(), span.end);
    return span;
}

std::string describe(const std::vector<Span> &spans) {
    std::string out;
    for (const Span &span: spans) {
        out += span.matched() ? std::to_string(span.begin) + ":" + std::to_string(span.end) : "x";
        out += ",";
    }
    return out;
}

std::filesystem::path golden_path() {
    return std::filesystem::path(__FILE__).parent_path() / "support" / "regex" / "golden.txt";
}

// Runs the whole file on one engine. An engine that cannot serve a case falls
// through to the Pike VM, as it does in production.
void check_engine(Engine engine, std::size_t dfa_budget = Scratch::kDefaultDfaBudgetBytes) {
    std::ifstream file(golden_path());
    ASSERT_TRUE(file.is_open()) << golden_path();

    Scratch scratch(dfa_budget);
    ScratchAccess::state(scratch).forced_engine = engine;
    std::map<std::pair<char, std::string>, std::optional<Regex>> cache;
    std::size_t cases = 0;
    std::size_t failures = 0;
    std::string line;
    while (std::getline(file, line)) {
        const std::vector<std::string_view> fields = split(line, '\t');
        ASSERT_EQ(fields.size(), 4U) << line;
        const char mode = fields[0][0];
        const std::string pattern = from_hex(fields[1]);
        const std::string text = from_hex(fields[2]);
        ++cases;

        auto [it, inserted] = cache.try_emplace({mode, pattern});
        if (inserted) {
            Options options;
            options.encoding = mode == 'B' ? Encoding::Bytes : Encoding::Utf8;
            auto compiled = Regex::compile(pattern, options);
            if (compiled) {
                it->second = std::move(*compiled);
            }
        }
        if (!it->second.has_value()) {
            ADD_FAILURE() << "does not compile: mode " << mode << " pattern " << pattern;
            if (++failures > 20) {
                break;
            }
            continue;
        }
        const Regex &regex = *it->second;

        const bool expect_match = fields[3] != "-";
        std::vector<Span> expected;
        if (expect_match) {
            for (const std::string_view field: split(fields[3], ',')) {
                expected.push_back(parse_span(field));
            }
        }

        std::vector<Span> actual(regex.capture_count() + 1);
        const MatchStatus status = regex.find(text, 0, actual, scratch);
        const bool test_match = regex.test(text, scratch) == MatchStatus::Match;
        const bool ok = status == (expect_match ? MatchStatus::Match : MatchStatus::NoMatch) &&
                        test_match == expect_match && (!expect_match || describe(actual) == describe(expected));
        if (!ok) {
            ADD_FAILURE() << "mode " << mode << " pattern [" << pattern << "] text [" << text << "]\n  expected "
                          << (expect_match ? describe(expected) : "no match") << "\n  actual   "
                          << (status == MatchStatus::Match ? describe(actual) : "no match") << " test=" << test_match;
            if (++failures > 20) {
                break;
            }
        }
    }
    EXPECT_GT(cases, 1000U);
}

TEST(RegexGolden, Dispatcher) { check_engine(Engine::Auto); }
TEST(RegexGolden, PikeVm) { check_engine(Engine::Pike); }
TEST(RegexGolden, BitState) { check_engine(Engine::BitState); }
TEST(RegexGolden, OnePass) { check_engine(Engine::OnePass); }
TEST(RegexGolden, Dfa) { check_engine(Engine::Dfa); }

// Memory pressure must only cost speed. These budgets force cache evictions and
// resets, and at zero every state has to be stepped without a cache at all.
TEST(RegexGolden, DfaUnderMemoryPressure) { check_engine(Engine::Dfa, 2048); }
TEST(RegexGolden, DfaWithoutMemory) { check_engine(Engine::Dfa, 0); }

} // namespace
