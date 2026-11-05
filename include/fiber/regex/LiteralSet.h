#ifndef FIBER_REGEX_LITERAL_SET_H
#define FIBER_REGEX_LITERAL_SET_H

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string_view>

#include "Regex.h"
#include "RegexSet.h"

namespace fiber::regex {

namespace detail {
struct CompiledLiterals;
struct LiteralBuilderState;
} // namespace detail

// Finds many literal strings in one pass, with an Aho-Corasick automaton. The cost
// per byte does not depend on how many literals there are. Immutable once built and
// safe to share between threads; it needs no Scratch.
class LiteralSet {
public:
    class Builder {
    public:
        // ignore_case folds ASCII letters only, as Regex does.
        explicit Builder(bool ignore_case = false);
        ~Builder();
        Builder(Builder &&) noexcept;
        Builder &operator=(Builder &&) noexcept;

        // Returns the id of the literal. An empty literal is a Syntax error: it would
        // match everywhere, which is never what a word list means.
        [[nodiscard]] std::expected<std::uint32_t, Error> add(std::string_view literal);
        [[nodiscard]] LiteralSet build() &&;

    private:
        std::unique_ptr<detail::LiteralBuilderState> state_;
    };

    // Where a stream is between two chunks. Matches can span them.
    struct Cursor {
        std::uint32_t node = 0;
    };

    [[nodiscard]] std::uint32_t size() const noexcept;
    [[nodiscard]] std::size_t memory_bytes() const noexcept;
    [[nodiscard]] MatchSet make_match_set() const { return MatchSet(size()); }

    // Whether any literal occurs in `text`, stopping at the first one.
    [[nodiscard]] bool contains_any(std::string_view text) const noexcept;
    // Adds every literal that occurs in `text` to `hits`.
    void scan(std::string_view text, MatchSet &hits) const noexcept;
    void feed(std::string_view chunk, Cursor &cursor, MatchSet &hits) const noexcept;

private:
    explicit LiteralSet(std::shared_ptr<const detail::CompiledLiterals> compiled) noexcept;

    std::shared_ptr<const detail::CompiledLiterals> compiled_;
};

} // namespace fiber::regex

#endif // FIBER_REGEX_LITERAL_SET_H
