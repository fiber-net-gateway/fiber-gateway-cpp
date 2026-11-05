// Checks that a RegexSet agrees with its patterns taken one at a time, and that a
// Scanner gives the same answer however the text is cut up, including when another
// stream resets the DFA cache between chunks. Input: [options][cut seed] then
// patterns separated by 0xFF 0x01, then 0xFF 0x00, then the text.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <utility>
#include <vector>

#include <fiber/regex/Regex.h>
#include <fiber/regex/RegexSet.h>
#include <fiber/regex/Scratch.h>

namespace {

using namespace fiber::regex;

constexpr std::size_t kMaxPatterns = 8;

void check_scanner(const RegexSet &set, std::string_view text, std::uint8_t seed, std::size_t dfa_budget,
                   const std::vector<bool> &expected) {
    Scratch scratch(dfa_budget);
    Scanner scanner(set);
    Scanner other(set); // shares the cache, so it can reset it under the first one
    WorkBudget budget{1U << 22U};

    std::uint32_t state = seed + 1U;
    std::size_t pos = 0;
    while (pos < text.size()) {
        state = state * 1103515245U + 12345U;
        const std::size_t length = 1 + ((state >> 16U) % 7U);
        const std::string_view chunk = text.substr(pos, length);
        if (scanner.feed(chunk, scratch, budget) == MatchStatus::BudgetExhausted) {
            return;
        }
        (void) other.feed(text.substr(0, length), scratch, budget);
        pos += length;
    }
    if (scanner.finish(scratch, budget) == MatchStatus::BudgetExhausted) {
        return;
    }
    for (std::uint32_t i = 0; i < expected.size(); ++i) {
        if (scanner.hits().contains(i) != expected[i]) {
            std::fprintf(stderr, "scanner disagrees on pattern %u, dfa budget %zu\n", i, dfa_budget);
            std::abort();
        }
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    if (size < 2) {
        return 0;
    }
    const std::uint8_t selector = data[0];
    const std::uint8_t seed = data[1];
    const std::string_view input(reinterpret_cast<const char *>(data + 2), size - 2);

    Options options;
    options.encoding = (selector & 1U) != 0 ? Encoding::Bytes : Encoding::Utf8;
    options.ignore_case = (selector & 2U) != 0;
    options.dot_matches_newline = (selector & 4U) != 0;
    options.multi_line = (selector & 8U) != 0;

    const std::size_t split = input.find("\xff\x00", 0, 2);
    std::string_view patterns = input.substr(0, split);
    const std::string_view text = split == std::string_view::npos ? std::string_view{} : input.substr(split + 2);

    // Patterns that do not compile are left out, keeping ids in step with `regexes`.
    RegexSet::Builder builder(options);
    std::vector<Regex> regexes;
    while (regexes.size() < kMaxPatterns) {
        const std::size_t end = patterns.find("\xff\x01", 0, 2);
        const std::string_view pattern = patterns.substr(0, end);
        auto regex = Regex::compile(pattern, options);
        if (regex && builder.add(pattern).has_value()) {
            regexes.push_back(std::move(*regex));
        }
        if (end == std::string_view::npos) {
            break;
        }
        patterns.remove_prefix(end + 2);
    }
    auto set = std::move(builder).build();
    if (!set) {
        return 0;
    }

    Scratch scratch;
    WorkBudget budget{1U << 22U};
    std::vector<bool> expected;
    for (const Regex &regex: regexes) {
        const MatchStatus status = regex.test(text, scratch, budget);
        if (status == MatchStatus::BudgetExhausted) {
            return 0;
        }
        expected.push_back(status == MatchStatus::Match);
    }

    MatchSet hits = set->make_match_set();
    if (set->scan(text, hits, scratch, budget) == MatchStatus::BudgetExhausted) {
        return 0;
    }
    for (std::uint32_t i = 0; i < expected.size(); ++i) {
        if (hits.contains(i) != expected[i]) {
            std::fprintf(stderr, "set disagrees on pattern %u\n", i);
            std::abort();
        }
    }

    check_scanner(*set, text, seed, Scratch::kDefaultDfaBudgetBytes, expected);
    check_scanner(*set, text, seed, 1500, expected);
    check_scanner(*set, text, seed, 0, expected);
    return 0;
}
