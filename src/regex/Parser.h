#ifndef FIBER_REGEX_PARSER_H
#define FIBER_REGEX_PARSER_H

#include <cstdint>
#include <expected>
#include <string_view>
#include <vector>

#include <fiber/regex/Regex.h>

#include "regex/Ast.h"

namespace fiber::regex::detail {

inline constexpr std::size_t kMaxPatternBytes = std::size_t{64} * 1024U;
inline constexpr int kMaxRepeat = 1000;
inline constexpr std::uint16_t kMaxHeight = 1000;

struct NamedCapture {
    std::string_view name{}; // points into the pattern
    std::uint32_t index = 0;
};

struct ParseResult {
    Node *root = nullptr;
    std::uint32_t capture_count = 0;
    std::vector<NamedCapture> names{};
};

// Whether parentheses capture. A RegexSet has no use for the groups.
enum class Captures : std::uint8_t { Keep, Ignore };

[[nodiscard]] std::expected<ParseResult, Error> parse(std::string_view pattern, const Options &options, Ast &ast,
                                                      Captures captures = Captures::Keep);

// Builds an alternation, factoring out common prefixes. Rewrites `subs`.
[[nodiscard]] Node *make_alternate(Ast &ast, std::vector<Node *> &subs, std::uint8_t flags);

// Largest rune the encoding can express.
[[nodiscard]] constexpr Rune rune_max(Encoding encoding) noexcept {
    return encoding == Encoding::Bytes ? Rune{0xFF} : kRuneMax;
}

} // namespace fiber::regex::detail

#endif // FIBER_REGEX_PARSER_H
