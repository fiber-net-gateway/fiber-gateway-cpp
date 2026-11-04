#include <fiber/regex/RegexSet.h>

#include <algorithm>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <fiber/regex/Scratch.h>

#include "regex/Ast.h"
#include "regex/Compiler.h"
#include "regex/Dfa.h"
#include "regex/Parser.h"
#include "regex/Program.h"
#include "regex/ScratchState.h"
#include "regex/Simplify.h"

namespace fiber::regex {

namespace detail {

struct SetBuilderState {
    struct Element {
        std::string pattern{};
        Node *root = nullptr;
    };

    Options options{};
    Ast ast{};
    std::vector<Element> elements{};
};

struct CompiledSet {
    Program program{};
    std::uint32_t size = 0;
};

struct ScannerState {
    std::shared_ptr<const CompiledSet> compiled{};
    MatchSet hits;
    DfaCursor cursor{};
};

} // namespace detail

bool MatchSet::any() const noexcept {
    return std::any_of(words_.begin(), words_.end(), [](std::uint64_t word) { return word != 0; });
}

void MatchSet::clear() noexcept { std::fill(words_.begin(), words_.end(), 0); }

RegexSet::Builder::Builder(const Options &options) : state_(std::make_unique<detail::SetBuilderState>()) {
    state_->options = options;
}

RegexSet::Builder::~Builder() = default;
RegexSet::Builder::Builder(Builder &&) noexcept = default;
RegexSet::Builder &RegexSet::Builder::operator=(Builder &&) noexcept = default;

std::expected<std::uint32_t, Error> RegexSet::Builder::add(std::string_view pattern) {
    if (pattern.size() > detail::kMaxPatternBytes) {
        return std::unexpected(Error{.code = ErrorCode::PatternTooLong, .offset = 0});
    }
    // The pattern text has to outlive the parse.
    std::string copy(pattern);
    auto parsed = detail::parse(copy, state_->options, state_->ast, detail::Captures::Ignore);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }

    // Concatenate the pattern with its match id.
    const auto id = static_cast<std::uint32_t>(state_->elements.size());
    detail::Node *have_match = state_->ast.make(detail::Op::HaveMatch, 0);
    have_match->cap = static_cast<std::int32_t>(id);
    std::vector<detail::Node *> parts;
    if (parsed->root->op == detail::Op::Concat) {
        parts = parsed->root->subs;
    } else {
        parts.push_back(parsed->root);
    }
    parts.push_back(have_match);
    detail::Node *root = state_->ast.make_concat(parts.data(), parts.size(), 0);
    state_->elements.push_back(detail::SetBuilderState::Element{.pattern = std::move(copy), .root = root});
    return id;
}

std::expected<RegexSet, Error> RegexSet::Builder::build() && {
    detail::SetBuilderState &state = *state_;

    // Sorting brings common prefixes together, for factoring to find. The ids stay
    // with their patterns.
    std::sort(state.elements.begin(), state.elements.end(),
              [](const auto &a, const auto &b) { return a.pattern < b.pattern; });

    std::vector<detail::Node *> subs;
    subs.reserve(state.elements.size());
    for (const detail::SetBuilderState::Element &element: state.elements) {
        subs.push_back(element.root);
    }

    Options options = state.options;
    constexpr std::uint32_t kSetFactor = 16;
    options.max_insts = options.max_insts > std::numeric_limits<std::uint32_t>::max() / kSetFactor
                                ? std::numeric_limits<std::uint32_t>::max()
                                : options.max_insts * kSetFactor;

    detail::Node *root = detail::make_alternate(state.ast, subs, 0);
    root = detail::simplify(state.ast, root, detail::rune_max(options.encoding));
    auto program = detail::compile_set(state.ast, root, options);
    if (!program) {
        return std::unexpected(program.error());
    }

    auto compiled = std::make_shared<detail::CompiledSet>();
    compiled->program = std::move(*program);
    compiled->size = static_cast<std::uint32_t>(state.elements.size());
    return RegexSet(std::move(compiled));
}

RegexSet::RegexSet(std::shared_ptr<const detail::CompiledSet> compiled) noexcept : compiled_(std::move(compiled)) {}

std::uint32_t RegexSet::size() const noexcept { return compiled_->size; }

MatchStatus RegexSet::scan(std::string_view text, MatchSet &hits, Scratch &scratch) const noexcept {
    WorkBudget budget;
    return scan(text, hits, scratch, budget);
}

MatchStatus RegexSet::scan(std::string_view text, MatchSet &hits, Scratch &scratch, WorkBudget &budget) const noexcept {
    detail::DfaRequest request;
    request.context = text;
    request.end = text.size();
    request.anchored = true;
    request.kind = detail::DfaSearchKind::ManyMatch;
    request.matches = &hits;
    return detail::dfa_search(compiled_->program, detail::ScratchAccess::state(scratch).dfa, request, budget).status;
}

Scanner::Scanner(const RegexSet &set) :
    state_(std::make_unique<detail::ScannerState>(detail::ScannerState{
            .compiled = set.compiled_, .hits = MatchSet(set.size()), .cursor = detail::DfaCursor{}})) {}

Scanner::~Scanner() = default;
Scanner::Scanner(Scanner &&) noexcept = default;
Scanner &Scanner::operator=(Scanner &&) noexcept = default;

MatchStatus Scanner::feed(std::string_view chunk, Scratch &scratch, WorkBudget &budget) noexcept {
    const MatchStatus status = detail::dfa_feed(state_->compiled->program, detail::ScratchAccess::state(scratch).dfa,
                                                state_->cursor, chunk, state_->hits, budget);
    if (status == MatchStatus::BudgetExhausted) {
        return status;
    }
    return state_->hits.any() ? MatchStatus::Match : MatchStatus::NoMatch;
}

MatchStatus Scanner::finish(Scratch &scratch, WorkBudget &budget) noexcept {
    const MatchStatus status = detail::dfa_finish(state_->compiled->program, detail::ScratchAccess::state(scratch).dfa,
                                                  state_->cursor, state_->hits, budget);
    if (status == MatchStatus::BudgetExhausted) {
        return status;
    }
    return state_->hits.any() ? MatchStatus::Match : MatchStatus::NoMatch;
}

const MatchSet &Scanner::hits() const noexcept { return state_->hits; }

} // namespace fiber::regex
