#include "regex/Ast.h"

#include <algorithm>

namespace fiber::regex::detail {

Node *Ast::make_star_plus_quest(Op op, Node *sub, std::uint8_t flags) {
    // Squash **, ++ and ??.
    if (op == sub->op && flags == sub->flags) {
        return sub;
    }
    // Squash *+, *?, +*, +?, ?* and ?+: they all mean *.
    if ((sub->op == Op::Star || sub->op == Op::Plus || sub->op == Op::Quest) && flags == sub->flags) {
        if (sub->op == Op::Star) {
            return sub;
        }
        Node *star = make(Op::Star, flags);
        star->subs.push_back(sub->subs[0]);
        update_height(*star);
        return star;
    }
    Node *node = make(op, flags);
    node->subs.push_back(sub);
    update_height(*node);
    return node;
}

Node *Ast::make_concat(Node *const *subs, std::size_t count, std::uint8_t flags) {
    if (count == 1) {
        return subs[0];
    }
    if (count == 0) {
        return make(Op::EmptyMatch, flags);
    }
    Node *node = make(Op::Concat, flags);
    node->subs.assign(subs, subs + count);
    update_height(*node);
    return node;
}

Node *Ast::make_alternate_no_factor(Node *const *subs, std::size_t count, std::uint8_t flags) {
    if (count == 1) {
        return subs[0];
    }
    if (count == 0) {
        return make(Op::NoMatch, flags);
    }
    Node *node = make(Op::Alternate, flags);
    node->subs.assign(subs, subs + count);
    update_height(*node);
    return node;
}

namespace {

bool top_equal(const Node *a, const Node *b) {
    if (a->op != b->op) {
        return false;
    }
    const auto flag_differs = [a, b](std::uint8_t flag) { return ((a->flags ^ b->flags) & flag) != 0; };
    switch (a->op) {
        case Op::Literal:
            return a->rune == b->rune && !flag_differs(kFoldCase);
        case Op::LiteralString:
            return a->runes == b->runes && !flag_differs(kFoldCase);
        case Op::Alternate:
        case Op::Concat:
            return a->subs.size() == b->subs.size();
        case Op::Star:
        case Op::Plus:
        case Op::Quest:
            return !flag_differs(kNonGreedy);
        case Op::Repeat:
            return !flag_differs(kNonGreedy) && a->min == b->min && a->max == b->max;
        case Op::Capture:
            return a->cap == b->cap && a->name == b->name;
        case Op::HaveMatch:
            return a->cap == b->cap;
        case Op::CharClass:
            return a->cc == b->cc;
        default:
            return true;
    }
}

} // namespace

bool ast_equal(const Node *a, const Node *b) {
    // Height is bounded by the parser, so recursion depth is too.
    if (!top_equal(a, b)) {
        return false;
    }
    for (std::size_t i = 0; i < a->subs.size(); ++i) {
        if (!ast_equal(a->subs[i], b->subs[i])) {
            return false;
        }
    }
    return true;
}

void update_height(Node &node) noexcept {
    std::uint16_t height = 0;
    for (const Node *sub: node.subs) {
        height = std::max(height, sub->height);
    }
    node.height = static_cast<std::uint16_t>(std::min<std::uint32_t>(height + 1U, 0xFFFFU));
}

} // namespace fiber::regex::detail
