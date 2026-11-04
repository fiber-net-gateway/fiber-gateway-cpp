// Regular expression parser, ported from RE2's parse.cc (see UPSTREAM.md).
//
// The parser is a precedence parser with a manual stack. Left parenthesis and
// vertical bar markers sit on the stack as nodes with marker opcodes. Scanning a
// vertical bar concatenates everything down to the nearest marker; scanning a
// right parenthesis then collapses the alternatives into one node.
#include "regex/Parser.h"

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <utility>

namespace fiber::regex::detail {

namespace {

// ---- Named character groups ----

constexpr std::array<RuneRange, 1> kDigit{{{'0', '9'}}};
constexpr std::array<RuneRange, 3> kPerlSpace{{{'\t', '\n'}, {'\f', '\r'}, {' ', ' '}}};
constexpr std::array<RuneRange, 4> kWord{{{'0', '9'}, {'A', 'Z'}, {'_', '_'}, {'a', 'z'}}};
constexpr std::array<RuneRange, 3> kAlnum{{{'0', '9'}, {'A', 'Z'}, {'a', 'z'}}};
constexpr std::array<RuneRange, 2> kAlpha{{{'A', 'Z'}, {'a', 'z'}}};
constexpr std::array<RuneRange, 1> kAscii{{{0x00, 0x7F}}};
constexpr std::array<RuneRange, 2> kBlank{{{'\t', '\t'}, {' ', ' '}}};
constexpr std::array<RuneRange, 2> kCntrl{{{0x00, 0x1F}, {0x7F, 0x7F}}};
constexpr std::array<RuneRange, 1> kGraph{{{0x21, 0x7E}}};
constexpr std::array<RuneRange, 1> kLower{{{'a', 'z'}}};
constexpr std::array<RuneRange, 1> kPrint{{{0x20, 0x7E}}};
constexpr std::array<RuneRange, 4> kPunct{{{0x21, 0x2F}, {0x3A, 0x40}, {0x5B, 0x60}, {0x7B, 0x7E}}};
constexpr std::array<RuneRange, 2> kPosixSpace{{{'\t', '\r'}, {' ', ' '}}};
constexpr std::array<RuneRange, 1> kUpper{{{'A', 'Z'}}};
constexpr std::array<RuneRange, 3> kXdigit{{{'0', '9'}, {'A', 'F'}, {'a', 'f'}}};

struct Group {
    std::string_view name;
    bool negated;
    std::span<const RuneRange> ranges;
};

constexpr std::array<Group, 6> kPerlGroups{{
        {.name = "\\d", .negated = false, .ranges = kDigit},
        {.name = "\\D", .negated = true, .ranges = kDigit},
        {.name = "\\s", .negated = false, .ranges = kPerlSpace},
        {.name = "\\S", .negated = true, .ranges = kPerlSpace},
        {.name = "\\w", .negated = false, .ranges = kWord},
        {.name = "\\W", .negated = true, .ranges = kWord},
}};

constexpr std::array<Group, 28> kPosixGroups{{
        {.name = "[:alnum:]", .negated = false, .ranges = kAlnum},
        {.name = "[:^alnum:]", .negated = true, .ranges = kAlnum},
        {.name = "[:alpha:]", .negated = false, .ranges = kAlpha},
        {.name = "[:^alpha:]", .negated = true, .ranges = kAlpha},
        {.name = "[:ascii:]", .negated = false, .ranges = kAscii},
        {.name = "[:^ascii:]", .negated = true, .ranges = kAscii},
        {.name = "[:blank:]", .negated = false, .ranges = kBlank},
        {.name = "[:^blank:]", .negated = true, .ranges = kBlank},
        {.name = "[:cntrl:]", .negated = false, .ranges = kCntrl},
        {.name = "[:^cntrl:]", .negated = true, .ranges = kCntrl},
        {.name = "[:digit:]", .negated = false, .ranges = kDigit},
        {.name = "[:^digit:]", .negated = true, .ranges = kDigit},
        {.name = "[:graph:]", .negated = false, .ranges = kGraph},
        {.name = "[:^graph:]", .negated = true, .ranges = kGraph},
        {.name = "[:lower:]", .negated = false, .ranges = kLower},
        {.name = "[:^lower:]", .negated = true, .ranges = kLower},
        {.name = "[:print:]", .negated = false, .ranges = kPrint},
        {.name = "[:^print:]", .negated = true, .ranges = kPrint},
        {.name = "[:punct:]", .negated = false, .ranges = kPunct},
        {.name = "[:^punct:]", .negated = true, .ranges = kPunct},
        {.name = "[:space:]", .negated = false, .ranges = kPosixSpace},
        {.name = "[:^space:]", .negated = true, .ranges = kPosixSpace},
        {.name = "[:upper:]", .negated = false, .ranges = kUpper},
        {.name = "[:^upper:]", .negated = true, .ranges = kUpper},
        {.name = "[:word:]", .negated = false, .ranges = kWord},
        {.name = "[:^word:]", .negated = true, .ranges = kWord},
        {.name = "[:xdigit:]", .negated = false, .ranges = kXdigit},
        {.name = "[:^xdigit:]", .negated = true, .ranges = kXdigit},
}};

const Group *lookup_group(std::span<const Group> groups, std::string_view name) noexcept {
    const auto it = std::find_if(groups.begin(), groups.end(), [name](const Group &g) { return g.name == name; });
    return it == groups.end() ? nullptr : &*it;
}

void add_group(CharClass &cc, const Group &group, std::uint8_t flags) {
    CharClass members;
    for (const RuneRange &range: group.ranges) {
        if ((flags & kFoldCase) != 0) {
            members.add_folded_range(range.lo, range.hi);
        } else {
            members.add_range(range.lo, range.hi);
        }
    }
    // Fold before negating: (?i)\W must also exclude what folds into \w.
    if (group.negated) {
        members.negate();
    }
    cc.add_class(members);
}

[[nodiscard]] constexpr bool is_ascii_letter(Rune r) noexcept {
    return (r >= 'A' && r <= 'Z') || (r >= 'a' && r <= 'z');
}
[[nodiscard]] constexpr bool is_ascii_digit(Rune r) noexcept { return r >= '0' && r <= '9'; }
[[nodiscard]] constexpr bool is_hex_digit(Rune r) noexcept {
    return is_ascii_digit(r) || (r >= 'A' && r <= 'F') || (r >= 'a' && r <= 'f');
}
[[nodiscard]] constexpr Rune hex_value(Rune r) noexcept {
    if (is_ascii_digit(r)) {
        return r - '0';
    }
    return (r >= 'a' ? r - 'a' : r - 'A') + 10;
}

// Escapes other engines give a meaning this one cannot provide in linear time,
// or that it leaves out on purpose. Reported apart from plain typos.
[[nodiscard]] constexpr bool is_unsupported_escape(Rune c) noexcept {
    switch (c) {
        case 'C':
        case 'G':
        case 'K':
        case 'N':
        case 'P':
        case 'R':
        case 'X':
        case 'Z':
        case 'g':
        case 'k':
        case 'p':
            return true;
        default:
            return c >= '1' && c <= '9';
    }
}

[[nodiscard]] constexpr bool is_marker(Op op) noexcept { return op == Op::LeftParen || op == Op::VerticalBar; }

// ---- Alternation factoring ----

// A factored prefix or merged class computed by one round of factoring, and the
// span of alternatives it replaces. nsuffix counts the suffixes left once those
// have been factored in turn.
struct Splice {
    Node *prefix;
    Node **sub;
    std::size_t nsub;
    std::size_t nsuffix = 0;
};

// The span being factored, its round and pending splices. Frames form an
// explicit stack so that factoring does not recurse.
struct Frame {
    Node **sub;
    std::size_t nsub;
    int round = 0;
    std::vector<Splice> splices{};
    std::size_t splice_index = 0;
};

// Returns the leading literal of `re`, or an empty span.
std::span<const Rune> leading_string(const Node *re, std::uint8_t &flags) noexcept {
    while (re->op == Op::Concat && !re->subs.empty()) {
        re = re->subs[0];
    }
    flags = re->flags & kFoldCase;
    if (re->op == Op::Literal) {
        return {&re->rune, 1};
    }
    if (re->op == Op::LiteralString) {
        return re->runes;
    }
    return {};
}

// Removes the first n leading runes from `re`, editing it in place. The tree
// has no shared nodes yet: only simplification creates those.
void remove_leading_string(Node *re, std::size_t n) {
    // Concats are flattened by the parser, so at most a couple are ever nested.
    std::array<Node *, 4> stack{};
    std::size_t depth = 0;
    while (re->op == Op::Concat) {
        if (depth < stack.size()) {
            stack[depth++] = re;
        }
        re = re->subs[0];
    }

    if (re->op == Op::Literal) {
        re->rune = 0;
        re->op = Op::EmptyMatch;
    } else if (re->op == Op::LiteralString) {
        if (n >= re->runes.size()) {
            re->runes.clear();
            re->op = Op::EmptyMatch;
        } else if (n == re->runes.size() - 1) {
            re->rune = re->runes.back();
            re->runes.clear();
            re->op = Op::Literal;
        } else {
            re->runes.erase(re->runes.begin(), re->runes.begin() + static_cast<std::ptrdiff_t>(n));
        }
    }

    // If re is now empty, concatenations might simplify too.
    while (depth > 0) {
        re = stack[--depth];
        if (re->subs[0]->op != Op::EmptyMatch) {
            continue;
        }
        if (re->subs.size() == 2) {
            *re = Node(*re->subs[1]);
        } else {
            re->subs.erase(re->subs.begin());
        }
    }
}

Node *leading_regexp(Node *re) noexcept {
    if (re->op == Op::EmptyMatch) {
        return nullptr;
    }
    if (re->op == Op::Concat && re->subs.size() >= 2) {
        return re->subs[0]->op == Op::EmptyMatch ? nullptr : re->subs[0];
    }
    return re;
}

Node *remove_leading_regexp(Ast &ast, Node *re) {
    if (re->op == Op::EmptyMatch) {
        return re;
    }
    if (re->op == Op::Concat && re->subs.size() >= 2) {
        if (re->subs[0]->op == Op::EmptyMatch) {
            return re;
        }
        if (re->subs.size() == 2) {
            return re->subs[1];
        }
        re->subs.erase(re->subs.begin());
        return re;
    }
    return ast.make(Op::EmptyMatch, re->flags);
}

// Round 1: factor out common literal prefixes.
void factor_round1(Ast &ast, Node **sub, std::size_t nsub, std::vector<Splice> &splices) {
    std::size_t start = 0;
    std::span<const Rune> rune;
    std::uint8_t rune_flags = 0;
    for (std::size_t i = 0; i <= nsub; ++i) {
        // Invariant: sub[start:i] all begin with `rune`.
        std::span<const Rune> rune_i;
        std::uint8_t rune_flags_i = 0;
        if (i < nsub) {
            rune_i = leading_string(sub[i], rune_flags_i);
            if (rune_flags_i == rune_flags) {
                std::size_t same = 0;
                while (same < rune.size() && same < rune_i.size() && rune[same] == rune_i[same]) {
                    ++same;
                }
                if (same > 0) {
                    rune = rune.first(same);
                    continue;
                }
            }
        }

        // End of a run: sub[start:i] share `rune`, but sub[i] does not start with it.
        if (i > start + 1) {
            Node *prefix = ast.make_literal_string(rune.data(), rune.size(), rune_flags);
            for (std::size_t j = start; j < i; ++j) {
                remove_leading_string(sub[j], rune.size());
            }
            splices.push_back(Splice{.prefix = prefix, .sub = sub + start, .nsub = i - start});
        }

        if (i < nsub) {
            start = i;
            rune = rune_i;
            rune_flags = rune_flags_i;
        }
    }
}

// Whether `first` may be pulled out of a run of alternatives. Quantified
// subexpressions are not safe: collapsing their paths through the automaton
// changes which match wins.
bool is_factorable_prefix(const Node *first) noexcept {
    switch (first->op) {
        case Op::BeginLine:
        case Op::EndLine:
        case Op::WordBoundary:
        case Op::NoWordBoundary:
        case Op::BeginText:
        case Op::EndText:
        case Op::CharClass:
        case Op::AnyChar:
            return true;
        case Op::Repeat: {
            const Op sub = first->subs[0]->op;
            return first->min == first->max && (sub == Op::Literal || sub == Op::CharClass || sub == Op::AnyChar);
        }
        default:
            return false;
    }
}

// Round 2: factor out common simple prefixes, just the first piece of each
// concatenation.
void factor_round2(Ast &ast, Node **sub, std::size_t nsub, std::vector<Splice> &splices) {
    std::size_t start = 0;
    Node *first = nullptr;
    for (std::size_t i = 0; i <= nsub; ++i) {
        Node *first_i = nullptr;
        if (i < nsub) {
            first_i = leading_regexp(sub[i]);
            if (first != nullptr && first_i != nullptr && is_factorable_prefix(first) && ast_equal(first, first_i)) {
                continue;
            }
        }

        if (i > start + 1) {
            for (std::size_t j = start; j < i; ++j) {
                sub[j] = remove_leading_regexp(ast, sub[j]);
            }
            splices.push_back(Splice{.prefix = first, .sub = sub + start, .nsub = i - start});
        }

        if (i < nsub) {
            start = i;
            first = first_i;
        }
    }
}

// Round 3: merge runs of literals and character classes.
void factor_round3(Ast &ast, Node **sub, std::size_t nsub, std::uint8_t flags, std::vector<Splice> &splices) {
    const auto mergeable = [](const Node *re) { return re->op == Op::Literal || re->op == Op::CharClass; };
    std::size_t start = 0;
    const Node *first = nullptr;
    for (std::size_t i = 0; i <= nsub; ++i) {
        const Node *first_i = nullptr;
        if (i < nsub) {
            first_i = sub[i];
            if (first != nullptr && mergeable(first) && mergeable(first_i)) {
                continue;
            }
        }

        if (i > start + 1) {
            Node *merged = ast.make(Op::CharClass, flags & static_cast<std::uint8_t>(~kFoldCase));
            for (std::size_t j = start; j < i; ++j) {
                const Node *re = sub[j];
                if (re->op == Op::CharClass) {
                    merged->cc.add_class(re->cc);
                } else if (re->fold_case()) {
                    merged->cc.add_folded_range(re->rune, re->rune);
                } else {
                    merged->cc.add_range(re->rune, re->rune);
                }
            }
            splices.push_back(Splice{.prefix = merged, .sub = sub + start, .nsub = i - start});
        }

        if (i < nsub) {
            start = i;
            first = first_i;
        }
    }
}

// Factors common prefixes out of an alternation: ABC|ABD|AEF|BCX|BCY becomes
// A(B(C|D)|EF)|BC(X|Y) and then A(B[CD]|EF)|BC[XY]. Rewrites `sub` in place and
// returns its new length.
std::size_t factor_alternation(Ast &ast, Node **top_sub, std::size_t top_nsub, std::uint8_t flags) {
    std::vector<Frame> stack;
    stack.push_back(Frame{.sub = top_sub, .nsub = top_nsub});

    for (;;) {
        Frame &frame = stack.back();
        if (frame.splices.empty()) {
            // Advance to the next round. This also covers the initial state.
            ++frame.round;
        } else if (frame.splice_index < frame.splices.size()) {
            // One more splice to factor: recurse logically.
            const Splice &splice = frame.splices[frame.splice_index];
            stack.push_back(Frame{.sub = splice.sub, .nsub = splice.nsub});
            continue;
        } else {
            // No more splices to factor: apply them.
            auto iter = frame.splices.begin();
            std::size_t out = 0;
            for (std::size_t i = 0; i < frame.nsub;) {
                while (frame.sub + i < iter->sub) {
                    frame.sub[out++] = frame.sub[i++];
                }
                if (frame.round == 3) {
                    frame.sub[out++] = iter->prefix;
                } else {
                    std::array<Node *, 2> parts{iter->prefix,
                                                ast.make_alternate_no_factor(iter->sub, iter->nsuffix, flags)};
                    frame.sub[out++] = ast.make_concat(parts.data(), parts.size(), flags);
                }
                i += iter->nsub;
                if (++iter == frame.splices.end()) {
                    while (i < frame.nsub) {
                        frame.sub[out++] = frame.sub[i++];
                    }
                }
            }
            frame.splices.clear();
            frame.nsub = out;
            ++frame.round;
        }

        switch (frame.round) {
            case 1:
                factor_round1(ast, frame.sub, frame.nsub, frame.splices);
                break;
            case 2:
                factor_round2(ast, frame.sub, frame.nsub, frame.splices);
                break;
            case 3:
                factor_round3(ast, frame.sub, frame.nsub, flags, frame.splices);
                break;
            default: {
                if (stack.size() == 1) {
                    return frame.nsub;
                }
                // Pop the stack and record how many suffixes remain.
                const std::size_t nsuffix = frame.nsub;
                stack.pop_back();
                Frame &parent = stack.back();
                parent.splices[parent.splice_index].nsuffix = nsuffix;
                ++parent.splice_index;
                continue;
            }
        }

        // Round 3 splices are final; earlier ones have suffixes left to factor.
        frame.splice_index = frame.splices.empty() || frame.round == 3 ? frame.splices.size() : 0;
    }
}

// Whether the combination of a repetition and the repetitions nested inside it
// stays within `limit` copies of the innermost expression. Only called when min
// or max is at least 2, so a subtree is rewalked at most log2(limit) times.
int repetition_budget(const Node *re, int limit) noexcept {
    int arg = limit;
    if (re->op == Op::Repeat) {
        const int count = re->max < 0 ? re->min : re->max;
        if (count > 0) {
            arg /= count;
        }
    }
    int result = arg;
    for (const Node *sub: re->subs) {
        result = std::min(result, repetition_budget(sub, arg));
    }
    return result;
}

// ---- Parser ----

class Parser {
public:
    Parser(std::string_view pattern, const Options &options, Ast &ast, Captures captures) noexcept :
        pattern_(pattern), ast_(ast), encoding_(options.encoding), rune_max_(rune_max(options.encoding)),
        captures_(captures) {
        flags_ = kOneLine;
        if (options.ignore_case) {
            flags_ |= kFoldCase;
        }
        if (options.dot_matches_newline) {
            flags_ |= kDotNewline;
        }
        if (options.multi_line) {
            flags_ &= static_cast<std::uint8_t>(~kOneLine);
        }
    }

    [[nodiscard]] std::expected<ParseResult, Error> run();

private:
    bool fail(ErrorCode code, std::size_t offset) noexcept {
        if (!error_) {
            error_ = Error{.code = code, .offset = static_cast<std::uint32_t>(offset)};
        }
        return false;
    }

    [[nodiscard]] std::size_t offset_of(std::string_view rest) const noexcept { return pattern_.size() - rest.size(); }

    bool next_rune(std::string_view &s, Rune &out) noexcept;

    bool push_regexp(Node *re);
    bool push_literal(Rune r);
    bool push_simple_op(Op op);
    bool push_dot();
    bool push_repeat_op(Op op, bool non_greedy, std::size_t offset);
    bool push_repetition(int min, int max, bool non_greedy, std::size_t offset);

    bool do_left_paren(std::string_view name);
    bool do_left_paren_no_capture();
    bool do_vertical_bar();
    bool do_right_paren(std::size_t offset);
    Node *do_finish();
    void do_concatenation();
    void do_alternation();
    void do_collapse(Op op);
    bool maybe_concat_string(int r, std::uint8_t flags);

    bool check_height(const Node &node, std::size_t offset) noexcept {
        return node.height <= kMaxHeight || fail(ErrorCode::NestingTooDeep, offset);
    }

    bool parse_escape(std::string_view &s, Rune &out);
    bool parse_perl_flags(std::string_view &s);
    bool parse_char_class(std::string_view &s, Node *&out);
    bool parse_class_character(std::string_view &s, Rune &out, std::size_t class_offset);
    bool parse_class_range(std::string_view &s, RuneRange &out, std::size_t class_offset);
    // Returns nullopt when `s` does not start a [:name:], true on success.
    std::optional<bool> parse_class_name(std::string_view &s, CharClass &cc);
    const Group *maybe_parse_perl_escape(std::string_view &s) const noexcept;

    bool parse_repeat_suffix(std::string_view &t, bool &non_greedy, bool last_was_repeat, std::size_t offset);

    std::string_view pattern_;
    Ast &ast_;
    Encoding encoding_;
    Rune rune_max_;
    Captures captures_;
    std::uint8_t flags_ = 0;
    std::vector<Node *> stack_{};
    std::int32_t capture_count_ = 0;
    std::vector<NamedCapture> names_{};
    std::optional<Error> error_{};
};

bool Parser::next_rune(std::string_view &s, Rune &out) noexcept {
    if (s.empty()) {
        return fail(ErrorCode::Syntax, pattern_.size());
    }
    if (encoding_ == Encoding::Bytes) {
        out = static_cast<unsigned char>(s[0]);
        s.remove_prefix(1);
        return true;
    }
    const std::size_t length = decode_utf8(s, out);
    if (length == 0) {
        return fail(ErrorCode::InvalidUtf8, offset_of(s));
    }
    s.remove_prefix(length);
    return true;
}

bool Parser::push_regexp(Node *re) {
    maybe_concat_string(-1, 0);

    // A class of one character is just a literal, which is a common idiom for
    // escaping ([.] instead of \.). [Aa] is likewise a case-folded literal.
    if (re->op == Op::CharClass) {
        re->cc.remove_above(rune_max_);
        const std::uint32_t count = re->cc.rune_count();
        if (count == 1) {
            const Rune r = re->cc.ranges()[0].lo;
            re = ast_.make(Op::Literal, flags_);
            re->rune = r;
        } else if (count == 2) {
            const Rune r = re->cc.ranges()[0].lo;
            if (r >= 'A' && r <= 'Z' && re->cc.contains(r + ('a' - 'A'))) {
                re = ast_.make(Op::Literal, flags_ | kFoldCase);
                re->rune = r + ('a' - 'A');
            }
        }
    }

    stack_.push_back(re);
    return true;
}

bool Parser::push_literal(Rune r) {
    if ((flags_ & kFoldCase) != 0 && is_ascii_letter(r)) {
        Node *re = ast_.make(Op::CharClass, flags_ & static_cast<std::uint8_t>(~kFoldCase));
        re->cc.add_folded_range(r, r);
        return push_regexp(re);
    }
    if (maybe_concat_string(r, flags_)) {
        return true;
    }
    Node *re = ast_.make(Op::Literal, flags_);
    re->rune = r;
    return push_regexp(re);
}

bool Parser::push_simple_op(Op op) { return push_regexp(ast_.make(op, flags_)); }

bool Parser::push_dot() {
    if ((flags_ & kDotNewline) != 0) {
        return push_simple_op(Op::AnyChar);
    }
    Node *re = ast_.make(Op::CharClass, flags_ & static_cast<std::uint8_t>(~kFoldCase));
    re->cc.add_range(0, '\n' - 1);
    re->cc.add_range('\n' + 1, rune_max_);
    return push_regexp(re);
}

bool Parser::push_repeat_op(Op op, bool non_greedy, std::size_t offset) {
    if (stack_.empty() || is_marker(stack_.back()->op)) {
        return fail(ErrorCode::Syntax, offset);
    }
    std::uint8_t flags = flags_;
    if (non_greedy) {
        flags ^= kNonGreedy;
    }

    Node *top = stack_.back();
    // Squash **, ++ and ??.
    if (op == top->op && flags == top->flags) {
        return true;
    }
    // Squash *+, *?, +*, +?, ?* and ?+: they all mean *.
    if ((top->op == Op::Star || top->op == Op::Plus || top->op == Op::Quest) && flags == top->flags) {
        top->op = Op::Star;
        return true;
    }

    Node *re = ast_.make(op, flags);
    re->subs.push_back(top);
    update_height(*re);
    stack_.back() = re;
    return check_height(*re, offset);
}

bool Parser::push_repetition(int min, int max, bool non_greedy, std::size_t offset) {
    if (max != -1 && max < min) {
        return fail(ErrorCode::Syntax, offset);
    }
    if (min > kMaxRepeat || max > kMaxRepeat) {
        return fail(ErrorCode::RepeatTooLarge, offset);
    }
    if (stack_.empty() || is_marker(stack_.back()->op)) {
        return fail(ErrorCode::Syntax, offset);
    }
    std::uint8_t flags = flags_;
    if (non_greedy) {
        flags ^= kNonGreedy;
    }
    Node *re = ast_.make(Op::Repeat, flags);
    re->min = min;
    re->max = max;
    re->subs.push_back(stack_.back());
    update_height(*re);
    stack_.back() = re;
    if (!check_height(*re, offset)) {
        return false;
    }
    if ((min >= 2 || max >= 2) && repetition_budget(re, kMaxRepeat) == 0) {
        return fail(ErrorCode::RepeatTooLarge, offset);
    }
    return true;
}

bool Parser::do_left_paren(std::string_view name) {
    Node *re = ast_.make(Op::LeftParen, flags_);
    re->cap = ++capture_count_;
    re->name = name;
    if (!name.empty()) {
        names_.push_back(NamedCapture{.name = name, .index = static_cast<std::uint32_t>(re->cap)});
    }
    return push_regexp(re);
}

bool Parser::do_left_paren_no_capture() {
    Node *re = ast_.make(Op::LeftParen, flags_);
    re->cap = -1;
    return push_regexp(re);
}

bool Parser::do_vertical_bar() {
    maybe_concat_string(-1, 0);
    do_concatenation();

    // Below the vertical bar is a list to alternate; above it, a list to
    // concatenate. We just did the concatenation, so either swap the result
    // below the vertical bar or push a new vertical bar on the stack.
    const std::size_t n = stack_.size();
    if (n >= 2 && stack_[n - 2]->op == Op::VerticalBar) {
        Node *r1 = stack_[n - 1];
        const auto subsumed = [](const Node *re) {
            return re->op == Op::Literal || re->op == Op::CharClass || re->op == Op::AnyChar;
        };
        if (n >= 3) {
            // AnyChar lets it subsume a neighbouring Literal, CharClass or AnyChar.
            Node *r3 = stack_[n - 3];
            if (r3->op == Op::AnyChar && subsumed(r1)) {
                stack_.pop_back();
                return true;
            }
            if (r1->op == Op::AnyChar && subsumed(r3)) {
                stack_[n - 3] = r1;
                stack_.pop_back();
                return true;
            }
        }
        std::swap(stack_[n - 1], stack_[n - 2]);
        return true;
    }
    return push_simple_op(Op::VerticalBar);
}

bool Parser::do_right_paren(std::size_t offset) {
    do_alternation();

    // The stack should be: LeftParen regexp.
    const std::size_t n = stack_.size();
    if (n < 2 || stack_[n - 2]->op != Op::LeftParen) {
        return fail(ErrorCode::Syntax, offset);
    }
    Node *r1 = stack_[n - 1];
    Node *re = stack_[n - 2];
    stack_.resize(n - 2);

    // Restore the flags from when the parenthesis opened.
    flags_ = re->flags;

    if (re->cap > 0) {
        re->op = Op::Capture;
        re->subs.push_back(r1);
        update_height(*re);
        if (!check_height(*re, offset)) {
            return false;
        }
    } else {
        re = r1;
    }
    return push_regexp(re);
}

Node *Parser::do_finish() {
    do_alternation();
    if (stack_.size() != 1) {
        fail(ErrorCode::Syntax, pattern_.size());
        return nullptr;
    }
    return stack_.back();
}

void Parser::do_collapse(Op op) {
    // Scan back to the marker, counting the children of the composite.
    std::size_t begin = stack_.size();
    std::size_t count = 0;
    while (begin > 0 && !is_marker(stack_[begin - 1]->op)) {
        --begin;
        count += stack_[begin]->op == op ? stack_[begin]->subs.size() : 1;
    }

    // Concat of one thing is that thing; so is alternate of one thing.
    if (stack_.size() - begin <= 1) {
        return;
    }

    // Construct op, flattening op of op.
    std::vector<Node *> subs;
    subs.reserve(count);
    for (std::size_t i = begin; i < stack_.size(); ++i) {
        Node *sub = stack_[i];
        if (sub->op == op) {
            subs.insert(subs.end(), sub->subs.begin(), sub->subs.end());
        } else {
            subs.push_back(sub);
        }
    }
    stack_.resize(begin);

    Node *re = op == Op::Alternate ? make_alternate(ast_, subs, flags_)
                                   : ast_.make_concat(subs.data(), subs.size(), flags_);
    stack_.push_back(re);
    check_height(*re, pattern_.size());
}

void Parser::do_concatenation() {
    if (stack_.empty() || is_marker(stack_.back()->op)) {
        // An empty concatenation is a special case.
        push_regexp(ast_.make(Op::EmptyMatch, flags_));
    }
    do_collapse(Op::Concat);
}

void Parser::do_alternation() {
    do_vertical_bar();
    // The stack top is now a VerticalBar.
    stack_.pop_back();
    do_collapse(Op::Alternate);
}

// If the top two stack elements are both literals or strings, collapses them
// into one string. Only called when another regexp is about to be pushed, so
// the topmost literal is not under consideration (ab* must not become (ab)*).
// If r >= 0, considers pushing the literal r and returns whether that happened.
bool Parser::maybe_concat_string(int r, std::uint8_t flags) {
    const std::size_t n = stack_.size();
    if (n < 2) {
        return false;
    }
    Node *re1 = stack_[n - 1];
    Node *re2 = stack_[n - 2];
    const auto is_literal = [](const Node *re) { return re->op == Op::Literal || re->op == Op::LiteralString; };
    if (!is_literal(re1) || !is_literal(re2)) {
        return false;
    }
    if (re1->fold_case() != re2->fold_case()) {
        return false;
    }

    if (re2->op == Op::Literal) {
        re2->op = Op::LiteralString;
        re2->runes.assign(1, re2->rune);
    }
    if (re1->op == Op::Literal) {
        re2->runes.push_back(re1->rune);
    } else {
        re2->runes.insert(re2->runes.end(), re1->runes.begin(), re1->runes.end());
        re1->runes.clear();
    }

    // Reuse re1 if possible.
    if (r >= 0) {
        re1->op = Op::Literal;
        re1->rune = r;
        re1->flags = flags;
        return true;
    }
    stack_.pop_back();
    return false;
}

// Parses a decimal integer without leading zeros.
bool parse_integer(std::string_view &s, int &out) noexcept {
    if (s.empty() || !is_ascii_digit(s[0])) {
        return false;
    }
    if (s.size() >= 2 && s[0] == '0' && is_ascii_digit(s[1])) {
        return false;
    }
    int value = 0;
    while (!s.empty() && is_ascii_digit(s[0])) {
        if (value >= 100000000) {
            return false; // avoid overflow
        }
        value = (value * 10) + (s[0] - '0');
        s.remove_prefix(1);
    }
    out = value;
    return true;
}

// Parses {1,2}, {2} or {2,}; {,2} is not a repetition. `sp` only advances on
// success, because a failed parse makes the { a literal instead of an error.
bool maybe_parse_repetition(std::string_view &sp, int &lo, int &hi) noexcept {
    std::string_view s = sp;
    if (s.empty() || s[0] != '{') {
        return false;
    }
    s.remove_prefix(1);
    if (!parse_integer(s, lo) || s.empty()) {
        return false;
    }
    if (s[0] == ',') {
        s.remove_prefix(1);
        if (s.empty()) {
            return false;
        }
        if (s[0] == '}') {
            hi = -1;
        } else if (!parse_integer(s, hi)) {
            return false;
        }
    } else {
        hi = lo;
    }
    if (s.empty() || s[0] != '}') {
        return false;
    }
    s.remove_prefix(1);
    sp = s;
    return true;
}

bool Parser::parse_escape(std::string_view &s, Rune &out) {
    const std::size_t begin = offset_of(s);
    if (s.size() == 1) {
        return fail(ErrorCode::Syntax, begin); // trailing backslash
    }
    s.remove_prefix(1);
    Rune c = 0;
    if (!next_rune(s, c)) {
        return false;
    }

    switch (c) {
        case 'n':
            out = '\n';
            return true;
        case 'r':
            out = '\r';
            return true;
        case 't':
            out = '\t';
            return true;
        case 'a':
            out = '\a';
            return true;
        case 'f':
            out = '\f';
            return true;
        case 'v':
            out = '\v';
            return true;
        case 'x':
            break;
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
            // A single non-zero octal digit is a backreference.
            if (s.empty() || s[0] < '0' || s[0] > '7') {
                return fail(ErrorCode::UnsupportedFeature, begin);
            }
            [[fallthrough]];
        case '0': {
            // Consume up to three octal digits; one is already in hand.
            Rune code = c - '0';
            for (int i = 0; i < 2 && !s.empty() && s[0] >= '0' && s[0] <= '7'; ++i) {
                code = (code * 8) + (s[0] - '0');
                s.remove_prefix(1);
            }
            if (code > rune_max_) {
                return fail(ErrorCode::Syntax, begin);
            }
            out = code;
            return true;
        }
        default:
            // Escaped punctuation is always itself; \q is not, unlike PCRE.
            if (c < kRuneSelf && !is_ascii_letter(c) && !is_ascii_digit(c)) {
                out = c;
                return true;
            }
            return fail(is_unsupported_escape(c) ? ErrorCode::UnsupportedFeature : ErrorCode::Syntax, begin);
    }

    // Hexadecimal escapes.
    if (s.empty()) {
        return fail(ErrorCode::Syntax, begin);
    }
    if (!next_rune(s, c)) {
        return false;
    }
    if (c == '{') {
        // Any number of hex digits in braces, at least one.
        if (s.empty()) {
            return fail(ErrorCode::Syntax, begin);
        }
        if (!next_rune(s, c)) {
            return false;
        }
        std::size_t digits = 0;
        Rune code = 0;
        while (is_hex_digit(c)) {
            ++digits;
            code = (code * 16) + hex_value(c);
            if (code > rune_max_ || s.empty()) {
                return fail(ErrorCode::Syntax, begin);
            }
            if (!next_rune(s, c)) {
                return false;
            }
        }
        if (c != '}' || digits == 0) {
            return fail(ErrorCode::Syntax, begin);
        }
        out = code;
        return true;
    }
    // Exactly two hex digits.
    if (s.empty()) {
        return fail(ErrorCode::Syntax, begin);
    }
    Rune c1 = 0;
    if (!next_rune(s, c1)) {
        return false;
    }
    if (!is_hex_digit(c) || !is_hex_digit(c1)) {
        return fail(ErrorCode::Syntax, begin);
    }
    out = (hex_value(c) * 16) + hex_value(c1);
    return true;
}

const Group *Parser::maybe_parse_perl_escape(std::string_view &s) const noexcept {
    if (s.size() < 2 || s[0] != '\\') {
        return nullptr;
    }
    const Group *group = lookup_group(kPerlGroups, s.substr(0, 2));
    if (group != nullptr) {
        s.remove_prefix(2);
    }
    return group;
}

std::optional<bool> Parser::parse_class_name(std::string_view &s, CharClass &cc) {
    if (s.size() < 2 || s[0] != '[' || s[1] != ':') {
        return std::nullopt;
    }
    const std::size_t end = s.find(":]", 2);
    if (end == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view name = s.substr(0, end + 2);
    const Group *group = lookup_group(kPosixGroups, name);
    if (group == nullptr) {
        return fail(ErrorCode::Syntax, offset_of(s));
    }
    s.remove_prefix(name.size());
    add_group(cc, *group, flags_);
    return true;
}

bool Parser::parse_class_character(std::string_view &s, Rune &out, std::size_t class_offset) {
    if (s.empty()) {
        return fail(ErrorCode::Syntax, class_offset);
    }
    if (s[0] == '\\') {
        return parse_escape(s, out);
    }
    return next_rune(s, out);
}

bool Parser::parse_class_range(std::string_view &s, RuneRange &out, std::size_t class_offset) {
    const std::size_t begin = offset_of(s);
    if (!parse_class_character(s, out.lo, class_offset)) {
        return false;
    }
    // [a-] means (a|-), so check for a following ].
    if (s.size() >= 2 && s[0] == '-' && s[1] != ']') {
        s.remove_prefix(1);
        if (!parse_class_character(s, out.hi, class_offset)) {
            return false;
        }
        if (out.hi < out.lo) {
            return fail(ErrorCode::Syntax, begin);
        }
    } else {
        out.hi = out.lo;
    }
    return true;
}

bool Parser::parse_char_class(std::string_view &s, Node *&out) {
    const std::size_t class_offset = offset_of(s);
    bool negated = false;
    Node *re = ast_.make(Op::CharClass, flags_ & static_cast<std::uint8_t>(~kFoldCase));
    s.remove_prefix(1); // '['
    if (!s.empty() && s[0] == '^') {
        s.remove_prefix(1);
        negated = true;
    }

    bool first = true; // ] is fine as the first character
    while (!s.empty() && (s[0] != ']' || first)) {
        first = false;

        // Look for [:alnum:].
        if (s.size() > 2 && s[0] == '[' && s[1] == ':') {
            const std::optional<bool> parsed = parse_class_name(s, re->cc);
            if (parsed.has_value()) {
                if (!*parsed) {
                    return false;
                }
                continue;
            }
        }

        // Look for \d and friends.
        if (const Group *group = maybe_parse_perl_escape(s); group != nullptr) {
            add_group(re->cc, *group, flags_);
            continue;
        }

        // Otherwise a regular character or range.
        RuneRange range;
        if (!parse_class_range(s, range, class_offset)) {
            return false;
        }
        if ((flags_ & kFoldCase) != 0) {
            re->cc.add_folded_range(range.lo, range.hi);
        } else {
            re->cc.add_range(range.lo, range.hi);
        }
    }
    if (s.empty()) {
        return fail(ErrorCode::Syntax, class_offset);
    }
    s.remove_prefix(1); // ']'

    if (negated) {
        re->cc.negate();
    }
    out = re;
    return true;
}

[[nodiscard]] bool is_valid_capture_name(std::string_view name) noexcept {
    return !name.empty() && std::all_of(name.begin(), name.end(),
                                        [](char c) { return is_ascii_letter(c) || is_ascii_digit(c) || c == '_'; });
}

bool Parser::parse_perl_flags(std::string_view &s) {
    // Caller checked that s starts with "(?".
    std::string_view t = s;
    const std::size_t begin = offset_of(s);

    if (t.size() >= 3) {
        const char c = t[2];
        // Lookahead, atomic groups, branch reset, comments, conditionals and recursion.
        if (c == '=' || c == '!' || c == '>' || c == '|' || c == '#' || c == '(' || c == 'R' || c == '&' || c == '+' ||
            is_ascii_digit(c)) {
            return fail(ErrorCode::UnsupportedFeature, begin);
        }
        // Lookbehind, and named backreferences or recursion.
        if (t.size() >= 4 &&
            ((c == '<' && (t[3] == '=' || t[3] == '!')) || (c == 'P' && (t[3] == '=' || t[3] == '>')))) {
            return fail(ErrorCode::UnsupportedFeature, begin);
        }
    }

    // Named captures: (?P<name>expr) and (?<name>expr).
    if ((t.size() > 4 && t[2] == 'P' && t[3] == '<') || (t.size() > 3 && t[2] == '<')) {
        const std::size_t name_begin = t[2] == 'P' ? 4 : 3;
        const std::size_t name_end = t.find('>', name_begin);
        if (name_end == std::string_view::npos) {
            return fail(ErrorCode::Syntax, begin);
        }
        const std::string_view name = t.substr(name_begin, name_end - name_begin);
        if (!is_valid_capture_name(name)) {
            return fail(ErrorCode::Syntax, begin);
        }
        const bool duplicate = std::any_of(names_.begin(), names_.end(),
                                           [name](const NamedCapture &named) { return named.name == name; });
        if (duplicate) {
            return fail(ErrorCode::Syntax, begin);
        }
        if (!(captures_ == Captures::Keep ? do_left_paren(name) : do_left_paren_no_capture())) {
            return false;
        }
        s.remove_prefix(name_end + 1);
        return true;
    }

    t.remove_prefix(2); // "(?"

    bool negated = false;
    bool saw_flags = false;
    std::uint8_t flags = flags_;
    const auto apply = [&flags, &negated, &saw_flags](std::uint8_t flag, bool set) {
        saw_flags = true;
        if (set != negated) {
            flags |= flag;
        } else {
            flags &= static_cast<std::uint8_t>(~flag);
        }
    };

    for (bool done = false; !done;) {
        if (t.empty()) {
            return fail(ErrorCode::Syntax, begin);
        }
        Rune c = 0;
        if (!next_rune(t, c)) {
            return false;
        }
        switch (c) {
            case 'i':
                apply(kFoldCase, true);
                break;
            case 'm': // the opposite of OneLine
                apply(kOneLine, false);
                break;
            case 's':
                apply(kDotNewline, true);
                break;
            case 'U':
                apply(kNonGreedy, true);
                break;
            case '-':
                if (negated) {
                    return fail(ErrorCode::Syntax, begin);
                }
                negated = true;
                saw_flags = false; // (?-) is an error
                break;
            case ':':
                if (!do_left_paren_no_capture()) {
                    return false;
                }
                done = true;
                break;
            case ')':
                done = true;
                break;
            default:
                return fail(ErrorCode::Syntax, begin);
        }
    }

    if (negated && !saw_flags) {
        return fail(ErrorCode::Syntax, begin);
    }
    flags_ = flags;
    s = t;
    return true;
}

bool Parser::parse_repeat_suffix(std::string_view &t, bool &non_greedy, bool last_was_repeat, std::size_t offset) {
    if (!t.empty() && t[0] == '?') {
        non_greedy = true;
        t.remove_prefix(1);
    }
    // A repeat straight after a repeat: a++ is possessive, a** is just invalid.
    if (last_was_repeat) {
        return fail(pattern_[offset] == '+' ? ErrorCode::UnsupportedFeature : ErrorCode::Syntax, offset);
    }
    return true;
}

std::expected<ParseResult, Error> Parser::run() {
    std::string_view t = pattern_;
    bool last_was_repeat = false;

    const auto failed = [this]() { return std::unexpected(*error_); };

    while (!t.empty()) {
        bool is_repeat = false;
        const std::size_t offset = offset_of(t);
        bool ok = true;

        switch (t[0]) {
            case '(':
                if (t.size() >= 2 && t[1] == '?') {
                    ok = parse_perl_flags(t);
                } else {
                    ok = captures_ == Captures::Keep ? do_left_paren({}) : do_left_paren_no_capture();
                    t.remove_prefix(1);
                }
                break;
            case '|':
                ok = do_vertical_bar();
                t.remove_prefix(1);
                break;
            case ')':
                ok = do_right_paren(offset);
                t.remove_prefix(1);
                break;
            case '^':
                ok = push_simple_op((flags_ & kOneLine) != 0 ? Op::BeginText : Op::BeginLine);
                t.remove_prefix(1);
                break;
            case '$':
                ok = push_simple_op((flags_ & kOneLine) != 0 ? Op::EndText : Op::EndLine);
                t.remove_prefix(1);
                break;
            case '.':
                ok = push_dot();
                t.remove_prefix(1);
                break;
            case '[': {
                Node *re = nullptr;
                ok = parse_char_class(t, re) && push_regexp(re);
                break;
            }
            case '*':
            case '+':
            case '?': {
                Op op = Op::Quest;
                if (t[0] == '*') {
                    op = Op::Star;
                } else if (t[0] == '+') {
                    op = Op::Plus;
                }
                bool non_greedy = false;
                t.remove_prefix(1);
                ok = parse_repeat_suffix(t, non_greedy, last_was_repeat, offset) &&
                     push_repeat_op(op, non_greedy, offset);
                is_repeat = true;
                break;
            }
            case '{': {
                int lo = 0;
                int hi = 0;
                if (!maybe_parse_repetition(t, lo, hi)) {
                    ok = push_literal('{');
                    t.remove_prefix(1);
                    break;
                }
                bool non_greedy = false;
                ok = parse_repeat_suffix(t, non_greedy, last_was_repeat, offset) &&
                     push_repetition(lo, hi, non_greedy, offset);
                is_repeat = true;
                break;
            }
            case '\\': {
                if (t.size() >= 2) {
                    const char next = t[1];
                    if (next == 'b' || next == 'B') {
                        ok = push_simple_op(next == 'b' ? Op::WordBoundary : Op::NoWordBoundary);
                        t.remove_prefix(2);
                        break;
                    }
                    if (next == 'A' || next == 'z') {
                        ok = push_simple_op(next == 'A' ? Op::BeginText : Op::EndText);
                        t.remove_prefix(2);
                        break;
                    }
                    if (next == 'Q') { // \Q ... \E: the ... is always literals
                        t.remove_prefix(2);
                        while (ok && !t.empty()) {
                            if (t.size() >= 2 && t[0] == '\\' && t[1] == 'E') {
                                t.remove_prefix(2);
                                break;
                            }
                            Rune r = 0;
                            ok = next_rune(t, r) && push_literal(r);
                        }
                        break;
                    }
                }
                if (const Group *group = maybe_parse_perl_escape(t); group != nullptr) {
                    Node *re = ast_.make(Op::CharClass, flags_ & static_cast<std::uint8_t>(~kFoldCase));
                    add_group(re->cc, *group, flags_);
                    ok = push_regexp(re);
                    break;
                }
                Rune r = 0;
                ok = parse_escape(t, r) && push_literal(r);
                break;
            }
            default: {
                Rune r = 0;
                ok = next_rune(t, r) && push_literal(r);
                break;
            }
        }

        if (!ok || error_) {
            return failed();
        }
        last_was_repeat = is_repeat;
    }

    Node *root = do_finish();
    if (root == nullptr || error_) {
        return failed();
    }
    return ParseResult{
            .root = root, .capture_count = static_cast<std::uint32_t>(capture_count_), .names = std::move(names_)};
}

} // namespace

std::expected<ParseResult, Error> parse(std::string_view pattern, const Options &options, Ast &ast, Captures captures) {
    return Parser(pattern, options, ast, captures).run();
}

Node *make_alternate(Ast &ast, std::vector<Node *> &subs, std::uint8_t flags) {
    if (subs.empty()) {
        return ast.make(Op::NoMatch, flags);
    }
    const std::size_t remaining = factor_alternation(ast, subs.data(), subs.size(), flags);
    return ast.make_alternate_no_factor(subs.data(), remaining, flags);
}

} // namespace fiber::regex::detail
