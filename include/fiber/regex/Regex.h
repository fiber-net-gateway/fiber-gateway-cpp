#ifndef FIBER_REGEX_REGEX_H
#define FIBER_REGEX_REGEX_H

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace fiber::regex {

class Scratch;

namespace detail {
struct Compiled;
} // namespace detail

// Utf8: patterns and text are Unicode code points. `.` and `[^x]` only match
// well-formed sequences, so one invalid byte splits a match.
// Bytes: one byte is one character. `.` and `[^x]` match any byte, which is what
// inspecting untrusted input needs: see feature/regex.md section 4.3.
enum class Encoding : std::uint8_t { Utf8, Bytes };

struct Options {
    Encoding encoding = Encoding::Utf8;
    bool ignore_case = false;
    bool dot_matches_newline = false;
    bool multi_line = false;
    std::uint32_t max_insts = 1U << 16U;
};

enum class ErrorCode : std::uint8_t {
    Syntax,
    UnsupportedFeature,
    InvalidUtf8,
    PatternTooLong,
    RepeatTooLarge,
    NestingTooDeep,
    ProgramTooLarge,
};

struct Error {
    ErrorCode code = ErrorCode::Syntax;
    std::uint32_t offset = 0; // byte offset into the pattern
};

struct Span {
    static constexpr std::size_t npos = std::numeric_limits<std::size_t>::max();

    std::size_t begin = npos; // groups that did not participate stay npos
    std::size_t end = npos;

    [[nodiscard]] bool matched() const noexcept { return begin != npos; }
};

enum class MatchStatus : std::uint8_t { NoMatch, Match, BudgetExhausted };

// Bounds the work one request may spend matching. A caller that runs out gets
// BudgetExhausted, never a silent NoMatch.
struct WorkBudget {
    std::uint64_t remaining = std::numeric_limits<std::uint64_t>::max();
};

// Immutable once compiled and safe to share between threads. Copies share the
// compiled program.
class Regex {
public:
    [[nodiscard]] static std::expected<Regex, Error> compile(std::string_view pattern, const Options &options = {});

    // Number of capturing groups, not counting $0.
    [[nodiscard]] std::uint32_t capture_count() const noexcept;
    [[nodiscard]] std::optional<std::uint32_t> capture_index(std::string_view name) const noexcept;

    // Reports whether `pattern` matches anywhere in `text`.
    [[nodiscard]] MatchStatus test(std::string_view text, Scratch &scratch) const noexcept;
    [[nodiscard]] MatchStatus test(std::string_view text, Scratch &scratch, WorkBudget &budget) const noexcept;

    // Finds the leftmost match at or after `start`. captures[0] receives $0 and
    // captures.size() decides how many groups are computed; groups the pattern
    // does not have stay unset. Bytes before `start` remain context for ^ and \b.
    [[nodiscard]] MatchStatus find(std::string_view text, std::size_t start, std::span<Span> captures,
                                   Scratch &scratch) const noexcept;
    [[nodiscard]] MatchStatus find(std::string_view text, std::size_t start, std::span<Span> captures, Scratch &scratch,
                                   WorkBudget &budget) const noexcept;

private:
    explicit Regex(std::shared_ptr<const detail::Compiled> compiled) noexcept;

    std::shared_ptr<const detail::Compiled> compiled_;
};

} // namespace fiber::regex

#endif // FIBER_REGEX_REGEX_H
