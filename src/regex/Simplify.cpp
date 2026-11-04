// Ported from RE2's simplify.cc (see UPSTREAM.md). RE2 walks with an explicit
// stack; the parser's height limit lets this version recurse instead.
#include "regex/Simplify.h"

#include <algorithm>
#include <array>
#include <vector>

#include <fiber/common/Assert.h>

namespace fiber::regex::detail {

namespace {

// Star, Plus, Quest or Repeat.
bool is_repeat_op(Op op) noexcept { return op == Op::Star || op == Op::Plus || op == Op::Quest || op == Op::Repeat; }

bool is_single_rune(Op op) noexcept { return op == Op::Literal || op == Op::CharClass || op == Op::AnyChar; }

// Whether r1 and r2 can merge into one repeat. Ensures the parse flags agree.
bool can_coalesce(const Node *r1, const Node *r2) {
    // r1 must be a repeat of something that matches a single rune.
    if (!is_repeat_op(r1->op) || !is_single_rune(r1->subs[0]->op)) {
        return false;
    }
    const Node *operand = r1->subs[0];
    // r2 is a repeat of that same operand...
    if (is_repeat_op(r2->op) && ast_equal(operand, r2->subs[0]) && r1->non_greedy() == r2->non_greedy()) {
        return true;
    }
    // ...or one occurrence of it...
    if (ast_equal(operand, r2)) {
        return true;
    }
    // ...or a literal string that begins with it.
    return operand->op == Op::Literal && r2->op == Op::LiteralString && r2->runes[0] == operand->rune &&
           operand->fold_case() == r2->fold_case();
}

// Merges *r1 and *r2. Afterwards they are an empty match and the merged repeat
// or, when only part of a literal string was consumed, the merged repeat and
// the rest of the string.
void do_coalesce(Ast &ast, Node *&r1, Node *&r2) {
    Node *merged = ast.make(Op::Repeat, r1->flags);
    merged->subs.push_back(r1->subs[0]);
    update_height(*merged);

    switch (r1->op) {
        case Op::Star:
            merged->min = 0;
            merged->max = -1;
            break;
        case Op::Plus:
            merged->min = 1;
            merged->max = -1;
            break;
        case Op::Quest:
            merged->min = 0;
            merged->max = 1;
            break;
        default: // Repeat
            merged->min = r1->min;
            merged->max = r1->max;
            break;
    }

    const auto add_fixed = [merged](int count) {
        merged->min += count;
        if (merged->max != -1) {
            merged->max += count;
        }
    };

    switch (r2->op) {
        case Op::Star:
            merged->max = -1;
            break;
        case Op::Plus:
            ++merged->min;
            merged->max = -1;
            break;
        case Op::Quest:
            if (merged->max != -1) {
                ++merged->max;
            }
            break;
        case Op::Repeat:
            merged->min += r2->min;
            if (r2->max == -1) {
                merged->max = -1;
            } else if (merged->max != -1) {
                merged->max += r2->max;
            }
            break;
        case Op::LiteralString: {
            // Work out how much of the literal string is consumed: at least one rune.
            const Rune r = r1->subs[0]->rune;
            std::size_t n = 1;
            while (n < r2->runes.size() && r2->runes[n] == r) {
                ++n;
            }
            add_fixed(static_cast<int>(n));
            if (n != r2->runes.size()) {
                const Node *rest = r2;
                r1 = merged;
                r2 = ast.make_literal_string(rest->runes.data() + n, rest->runes.size() - n, rest->flags);
                return;
            }
            break;
        }
        default: // Literal, CharClass or AnyChar
            add_fixed(1);
            break;
    }

    r1 = ast.make(Op::EmptyMatch, 0);
    r2 = merged;
}

Node *coalesce(Ast &ast, Node *re) {
    for (Node *&sub: re->subs) {
        sub = coalesce(ast, sub);
    }
    if (re->op != Op::Concat) {
        return re;
    }

    bool any = false;
    for (std::size_t i = 0; i + 1 < re->subs.size(); ++i) {
        if (can_coalesce(re->subs[i], re->subs[i + 1])) {
            do_coalesce(ast, re->subs[i], re->subs[i + 1]);
            any = true;
        }
    }
    if (any) {
        std::erase_if(re->subs, [](const Node *sub) { return sub->op == Op::EmptyMatch; });
    }
    return re;
}

bool is_empty_op(const Node *re) noexcept {
    switch (re->op) {
        case Op::BeginLine:
        case Op::EndLine:
        case Op::WordBoundary:
        case Op::NoWordBoundary:
        case Op::BeginText:
        case Op::EndText:
            return true;
        default:
            return false;
    }
}

// Rewrites re{min,max} in terms of *, + and ?. The result shares `re` between
// the copies, so (x){2} captures into the same group twice.
Node *simplify_repeat(Ast &ast, Node *re, int min, int max, std::uint8_t flags) {
    // A repeat of empty-width ops cannot match anything new after the first pass.
    if (is_empty_op(re) || ((re->op == Op::Concat || re->op == Op::Alternate) &&
                            std::all_of(re->subs.begin(), re->subs.end(), is_empty_op))) {
        min = std::min(min, 1);
        max = std::min(max, 1);
    }

    // x{n,} means at least n matches of x.
    if (max == -1) {
        if (min == 0) {
            return ast.make_star_plus_quest(Op::Star, re, flags);
        }
        if (min == 1) {
            return ast.make_star_plus_quest(Op::Plus, re, flags);
        }
        // x{4,} is xxxx+.
        std::vector<Node *> subs(static_cast<std::size_t>(min), re);
        subs.back() = ast.make_star_plus_quest(Op::Plus, re, flags);
        return ast.make_concat(subs.data(), subs.size(), flags);
    }

    if (min == 0 && max == 0) {
        return ast.make(Op::EmptyMatch, flags);
    }
    if (min == 1 && max == 1) {
        return re;
    }

    // x{n,m} is n copies of x and m-n copies of x?. The machine does less work
    // if the optional copies nest: x{2,5} = xx(x(x(x)?)?)?
    Node *result = nullptr;
    if (min > 0) {
        const std::vector<Node *> subs(static_cast<std::size_t>(min), re);
        result = ast.make_concat(subs.data(), subs.size(), flags);
    }
    if (max > min) {
        Node *suffix = ast.make_star_plus_quest(Op::Quest, re, flags);
        for (int i = min + 1; i < max; ++i) {
            const std::array<Node *, 2> parts{re, suffix};
            suffix = ast.make_star_plus_quest(Op::Quest, ast.make_concat(parts.data(), parts.size(), flags), flags);
        }
        if (result == nullptr) {
            result = suffix;
        } else {
            const std::array<Node *, 2> parts{result, suffix};
            result = ast.make_concat(parts.data(), parts.size(), flags);
        }
    }
    FIBER_ASSERT(result != nullptr);
    return result;
}

Node *simplify_node(Ast &ast, Node *re, Rune max) {
    switch (re->op) {
        case Op::Concat:
        case Op::Alternate:
        case Op::Capture:
            for (Node *&sub: re->subs) {
                sub = simplify_node(ast, sub, max);
            }
            return re;

        case Op::Star:
        case Op::Plus:
        case Op::Quest: {
            Node *sub = simplify_node(ast, re->subs[0], max);
            // Repeat the empty string as much as you want: it is still empty.
            if (sub->op == Op::EmptyMatch) {
                return sub;
            }
            // These are idempotent if the flags agree.
            if (re->op == sub->op && re->flags == sub->flags) {
                return sub;
            }
            re->subs[0] = sub;
            return re;
        }

        case Op::Repeat: {
            Node *sub = simplify_node(ast, re->subs[0], max);
            if (sub->op == Op::EmptyMatch) {
                return sub;
            }
            return simplify_repeat(ast, sub, re->min, re->max, re->flags);
        }

        case Op::CharClass: {
            if (re->cc.empty()) {
                return ast.make(Op::NoMatch, re->flags);
            }
            const std::vector<RuneRange> &ranges = re->cc.ranges();
            if (ranges.size() == 1 && ranges[0].lo == 0 && ranges[0].hi == max) {
                return ast.make(Op::AnyChar, re->flags);
            }
            return re;
        }

        default:
            return re;
    }
}

} // namespace

Node *simplify(Ast &ast, Node *root, Rune max) { return simplify_node(ast, coalesce(ast, root), max); }

} // namespace fiber::regex::detail
