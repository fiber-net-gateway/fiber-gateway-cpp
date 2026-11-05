#ifndef FIBER_REGEX_AST_H
#define FIBER_REGEX_AST_H

#include <cstdint>
#include <deque>
#include <string_view>
#include <vector>

#include "regex/CharClass.h"
#include "regex/Rune.h"

namespace fiber::regex::detail {

enum class Op : std::uint8_t {
    NoMatch,
    EmptyMatch,
    Literal,
    LiteralString,
    Concat,
    Alternate,
    Star,
    Plus,
    Quest,
    Repeat,
    Capture,
    AnyChar,
    CharClass,
    BeginLine,
    EndLine,
    WordBoundary,
    NoWordBoundary,
    BeginText,
    EndText,
    HaveMatch,
    // Parse stack markers. They never appear in a finished tree.
    LeftParen,
    VerticalBar,
};

enum ParseFlag : std::uint8_t {
    kFoldCase = 1U << 0U,
    kDotNewline = 1U << 1U,
    kOneLine = 1U << 2U, // ^ and $ only match at the ends of the text
    kNonGreedy = 1U << 3U,
};

// Nodes live in an Ast arena. Simplification shares subtrees, so the result is
// a DAG and every rewrite must build new nodes instead of editing shared ones.
struct Node {
    Op op = Op::EmptyMatch;
    std::uint8_t flags = 0;
    std::uint16_t height = 1;
    std::int32_t cap = 0; // Capture: group index. LeftParen: index, or -1 for (?:
    std::int32_t min = 0;
    std::int32_t max = 0; // -1 means unbounded
    Rune rune = 0;
    std::vector<Node *> subs{};
    std::vector<Rune> runes{};
    CharClass cc{};
    std::string_view name{};

    [[nodiscard]] bool non_greedy() const noexcept { return (flags & kNonGreedy) != 0; }
    [[nodiscard]] bool fold_case() const noexcept { return (flags & kFoldCase) != 0; }
};

class Ast {
public:
    Node *make(Op op, std::uint8_t flags) {
        Node &node = nodes_.emplace_back();
        node.op = op;
        node.flags = flags;
        return &node;
    }

    Node *make_literal_string(const Rune *runes, std::size_t count, std::uint8_t flags) {
        if (count == 1) {
            Node *node = make(Op::Literal, flags);
            node->rune = runes[0];
            return node;
        }
        Node *node = make(Op::LiteralString, flags);
        node->runes.assign(runes, runes + count);
        return node;
    }

    Node *make_star_plus_quest(Op op, Node *sub, std::uint8_t flags);
    Node *make_concat(Node *const *subs, std::size_t count, std::uint8_t flags);
    Node *make_alternate_no_factor(Node *const *subs, std::size_t count, std::uint8_t flags);

private:
    std::deque<Node> nodes_{};
};

// Structural equality, ignoring the parse flags that do not change meaning.
[[nodiscard]] bool ast_equal(const Node *a, const Node *b);

// Recomputes `height` from the immediate children.
void update_height(Node &node) noexcept;

} // namespace fiber::regex::detail

#endif // FIBER_REGEX_AST_H
