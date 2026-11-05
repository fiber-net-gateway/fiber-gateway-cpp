// Runs one pattern on one text through every engine and aborts if any two disagree.
// That covers Pike VM, BitState, OnePass and the DFA, the last also with too little
// memory to keep any state, which forces it to step without a cache. The first byte
// selects the options; pattern and text are split at the first 0xFF 0x00 pair.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <utility>

#include <fiber/regex/Regex.h>
#include <fiber/regex/Scratch.h>

#include "regex/ScratchState.h"

namespace {

using namespace fiber::regex;
using detail::Engine;
using detail::ScratchAccess;

constexpr std::size_t kSpans = 6;

struct Outcome {
    MatchStatus status = MatchStatus::NoMatch;
    MatchStatus test_status = MatchStatus::NoMatch;
    std::array<Span, kSpans> spans{};

    bool operator==(const Outcome &other) const noexcept {
        if (status != other.status || test_status != other.test_status) {
            return false;
        }
        for (std::size_t i = 0; i < kSpans; ++i) {
            if (spans[i].begin != other.spans[i].begin || spans[i].end != other.spans[i].end) {
                return false;
            }
        }
        return true;
    }
};

Outcome run(const Regex &regex, std::string_view text, Engine engine, std::size_t dfa_budget) {
    Scratch scratch(dfa_budget);
    ScratchAccess::state(scratch).forced_engine = engine;

    Outcome outcome;
    WorkBudget budget{1U << 22U};
    outcome.status = regex.find(text, 0, outcome.spans, scratch, budget);
    outcome.test_status = regex.test(text, scratch, budget);
    return outcome;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    if (size < 1) {
        return 0;
    }
    const std::uint8_t selector = data[0];
    const std::string_view input(reinterpret_cast<const char *>(data + 1), size - 1);

    Options options;
    options.encoding = (selector & 1U) != 0 ? Encoding::Bytes : Encoding::Utf8;
    options.ignore_case = (selector & 2U) != 0;
    options.dot_matches_newline = (selector & 4U) != 0;
    options.multi_line = (selector & 8U) != 0;

    const std::size_t split = input.find("\xff\x00", 0, 2);
    const std::string_view pattern = input.substr(0, split);
    const std::string_view text = split == std::string_view::npos ? std::string_view{} : input.substr(split + 2);

    auto regex = Regex::compile(pattern, options);
    if (!regex) {
        return 0;
    }

    const Outcome expected = run(*regex, text, Engine::Pike, Scratch::kDefaultDfaBudgetBytes);
    if (expected.status == MatchStatus::BudgetExhausted || expected.test_status == MatchStatus::BudgetExhausted) {
        return 0; // engines spend differently, so there is nothing to compare
    }

    const std::array<std::pair<Engine, std::size_t>, 6> others{{
            {Engine::BitState, Scratch::kDefaultDfaBudgetBytes},
            {Engine::OnePass, Scratch::kDefaultDfaBudgetBytes},
            {Engine::Dfa, Scratch::kDefaultDfaBudgetBytes},
            {Engine::Dfa, 1500},
            {Engine::Dfa, 0},
            {Engine::Auto, Scratch::kDefaultDfaBudgetBytes},
    }};
    for (const auto &[engine, dfa_budget]: others) {
        const Outcome actual = run(*regex, text, engine, dfa_budget);
        if (actual.status == MatchStatus::BudgetExhausted || actual.test_status == MatchStatus::BudgetExhausted) {
            continue;
        }
        if (!(actual == expected)) {
            std::fprintf(stderr, "engine %d, dfa budget %zu disagrees with the Pike VM\n", static_cast<int>(engine),
                         dfa_budget);
            std::abort();
        }
    }
    return 0;
}
