#include <fiber/regex/Regex.h>

#include <algorithm>
#include <string>
#include <utility>

#include <fiber/regex/Scratch.h>

#include "regex/Ast.h"
#include "regex/BitState.h"
#include "regex/Compiled.h"
#include "regex/Compiler.h"
#include "regex/OnePass.h"
#include "regex/Parser.h"
#include "regex/PikeVm.h"
#include "regex/Program.h"
#include "regex/ScratchState.h"
#include "regex/Search.h"
#include "regex/Simplify.h"

namespace fiber::regex {

namespace {

using detail::Engine;

bool can_use_one_pass(const detail::Program &program, const detail::SearchRequest &request) noexcept {
    const bool anchored = request.anchored || request.full_match || program.anchor_start();
    return anchored && program.is_one_pass() && request.captures.size() <= detail::kMaxOnePassCapture;
}

bool can_use_bit_state(const detail::Program &program, const detail::SearchRequest &request) noexcept {
    return program.can_bit_state() && request.end - request.begin <= program.bit_state_text_max_size();
}

// Picks the fastest engine that can find captures, in the order of RE2.
MatchStatus run_capture_engine(const detail::Program &program, detail::ScratchState &state,
                               const detail::SearchRequest &request, WorkBudget &budget) noexcept {
    const Engine forced = state.forced_engine;
    const bool any = forced == Engine::Auto || forced == Engine::Dfa;
    if ((any || forced == Engine::OnePass) && can_use_one_pass(program, request)) {
        return detail::one_pass_search(program, request, budget);
    }
    if ((any || forced == Engine::BitState) && can_use_bit_state(program, request)) {
        return detail::bit_state_search(program, state.bit_state, request, budget);
    }
    return detail::pike_search(program, state.pike, request, budget);
}

// Whether a short text is better off skipping the DFA. With captures needed anyway
// OnePass or BitState are fast enough, and on tiny texts OnePass beats even the DFA.
bool skip_dfa(const detail::Program &program, const detail::SearchRequest &request, Engine forced) noexcept {
    if (forced == Engine::Dfa) {
        return false;
    }
    const std::size_t length = request.end - request.begin;
    const std::size_t ncap = request.captures.size();
    if (can_use_one_pass(program, request) && length <= 4096 && (ncap > 1 || length <= 16)) {
        return true;
    }
    return ncap > 1 && can_use_bit_state(program, request);
}

// Finds $0 with the DFAs, then the groups, if any, with an engine that can. Ported
// from RE2::Match, minus the fallbacks for a DFA that gave up.
MatchStatus search(const detail::Compiled &compiled, detail::ScratchState &state, detail::SearchRequest request,
                   WorkBudget &budget) noexcept {
    const detail::Program &program = compiled.program;
    const Engine forced = state.forced_engine;
    if (forced == Engine::Pike || forced == Engine::BitState || forced == Engine::OnePass) {
        return run_capture_engine(program, state, request, budget);
    }

    if (program.start() == 0) {
        return MatchStatus::NoMatch;
    }
    if (program.anchor_start() && request.begin != 0) {
        return MatchStatus::NoMatch;
    }
    if (program.anchor_end() && request.end != request.context.size()) {
        return MatchStatus::NoMatch;
    }

    const std::size_t ncap = request.captures.size();
    const bool want_position = ncap > 0;

    detail::DfaRequest dfa;
    dfa.context = request.context;
    dfa.begin = request.begin;
    dfa.end = request.end;
    dfa.want_position = want_position;

    Span match{.begin = request.begin, .end = request.end};

    if (program.anchor_start()) {
        // Anchored: the forward DFA alone pins the match down.
        if (skip_dfa(program, request, forced)) {
            return run_capture_engine(program, state, request, budget);
        }
        dfa.anchored = true;
        dfa.kind = program.anchor_end() ? detail::DfaSearchKind::FullMatch : detail::DfaSearchKind::FirstMatch;
        const detail::DfaResult result = detail::dfa_search(program, state.dfa, dfa, budget);
        if (result.status != MatchStatus::Match) {
            return result.status;
        }
        match.end = result.position;
    } else if (!compiled.reverse.has_value()) {
        return detail::pike_search(program, state.pike, request, budget);
    } else if (program.anchor_end()) {
        // A special case: the match is known to end at the end of the text, so the
        // reverse DFA can say whether there is one and where it starts.
        dfa.anchored = true;
        dfa.kind = detail::DfaSearchKind::LongestMatch;
        const detail::DfaResult result = detail::dfa_search(*compiled.reverse, state.dfa, dfa, budget);
        if (result.status != MatchStatus::Match) {
            return result.status;
        }
        match.begin = result.position;
    } else {
        if (skip_dfa(program, request, forced)) {
            return run_capture_engine(program, state, request, budget);
        }
        dfa.kind = detail::DfaSearchKind::FirstMatch;
        const detail::DfaResult result = detail::dfa_search(program, state.dfa, dfa, budget);
        if (result.status != MatchStatus::Match) {
            return result.status;
        }
        if (!want_position) {
            return MatchStatus::Match;
        }
        match.end = result.position;

        // That gives the end of the match. Run backward from there to find the
        // longest possible match, which is where it started.
        detail::DfaRequest reverse = dfa;
        reverse.end = match.end;
        reverse.anchored = true;
        reverse.kind = detail::DfaSearchKind::LongestMatch;
        reverse.want_position = true;
        const detail::DfaResult start = detail::dfa_search(*compiled.reverse, state.dfa, reverse, budget);
        if (start.status != MatchStatus::Match) {
            return start.status;
        }
        match.begin = start.position;
    }

    if (ncap == 0) {
        return MatchStatus::Match;
    }
    if (ncap == 1) {
        request.captures[0] = match;
        return MatchStatus::Match;
    }

    // The exact location is known, so an anchored full match finds the groups.
    request.begin = match.begin;
    request.end = match.end;
    request.full_match = true;
    return run_capture_engine(program, state, request, budget);
}

} // namespace

Regex::Regex(std::shared_ptr<const detail::Compiled> compiled) noexcept : compiled_(std::move(compiled)) {}

std::expected<Regex, Error> Regex::compile(std::string_view pattern, const Options &options) {
    if (pattern.size() > detail::kMaxPatternBytes) {
        return std::unexpected(Error{.code = ErrorCode::PatternTooLong, .offset = 0});
    }

    detail::Ast ast;
    auto parsed = detail::parse(pattern, options, ast);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }

    detail::Node *root = detail::simplify(ast, parsed->root, detail::rune_max(options.encoding));
    auto program = detail::compile_program(ast, root, options, false);
    if (!program) {
        return std::unexpected(program.error());
    }

    auto compiled = std::make_shared<detail::Compiled>();
    compiled->program = std::move(*program);
    if (auto reverse = detail::compile_program(ast, root, options, true); reverse) {
        compiled->reverse = std::move(*reverse);
    }
    compiled->capture_count = parsed->capture_count;
    compiled->names.reserve(parsed->names.size());
    for (const detail::NamedCapture &named: parsed->names) {
        compiled->names.push_back(detail::CompiledName{.name = std::string(named.name), .index = named.index});
    }
    return Regex(std::move(compiled));
}

std::uint32_t Regex::capture_count() const noexcept { return compiled_->capture_count; }

std::optional<std::uint32_t> Regex::capture_index(std::string_view name) const noexcept {
    const auto &names = compiled_->names;
    const auto it = std::find_if(names.begin(), names.end(),
                                 [name](const detail::CompiledName &named) { return named.name == name; });
    if (it == names.end()) {
        return std::nullopt;
    }
    return it->index;
}

MatchStatus Regex::test(std::string_view text, Scratch &scratch) const noexcept {
    WorkBudget budget;
    return test(text, scratch, budget);
}

MatchStatus Regex::test(std::string_view text, Scratch &scratch, WorkBudget &budget) const noexcept {
    detail::SearchRequest request;
    request.context = text;
    request.end = text.size();
    request.earliest = true;
    return search(*compiled_, detail::ScratchAccess::state(scratch), request, budget);
}

MatchStatus Regex::find(std::string_view text, std::size_t start, std::span<Span> captures,
                        Scratch &scratch) const noexcept {
    WorkBudget budget;
    return find(text, start, captures, scratch, budget);
}

MatchStatus Regex::find(std::string_view text, std::size_t start, std::span<Span> captures, Scratch &scratch,
                        WorkBudget &budget) const noexcept {
    std::fill(captures.begin(), captures.end(), Span{});
    if (start > text.size()) {
        return MatchStatus::NoMatch;
    }

    detail::SearchRequest request;
    request.context = text;
    request.begin = start;
    request.end = text.size();
    // Registers past the last group would never be written.
    request.captures =
            captures.first(std::min<std::size_t>(captures.size(), std::size_t{compiled_->capture_count} + 1));
    return search(*compiled_, detail::ScratchAccess::state(scratch), request, budget);
}

} // namespace fiber::regex
