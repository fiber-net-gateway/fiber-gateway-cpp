#ifndef FIBER_REGEX_REGEX_SET_H
#define FIBER_REGEX_REGEX_SET_H

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string_view>
#include <vector>

#include "Regex.h"

namespace fiber::regex {

namespace detail {
struct CompiledSet;
struct SetBuilderState;
struct ScannerState;
} // namespace detail

// Which patterns of a RegexSet matched.
class MatchSet {
public:
    explicit MatchSet(std::uint32_t size) : size_(size), words_((std::size_t{size} + 63U) / 64U, 0) {}

    [[nodiscard]] std::uint32_t size() const noexcept { return size_; }
    [[nodiscard]] bool contains(std::uint32_t id) const noexcept {
        return ((words_[id / 64U] >> (id % 64U)) & 1U) != 0;
    }
    [[nodiscard]] bool any() const noexcept;

    void insert(std::uint32_t id) noexcept { words_[id / 64U] |= std::uint64_t{1} << (id % 64U); }
    void clear() noexcept;

private:
    std::uint32_t size_;
    std::vector<std::uint64_t> words_;
};

// Matches many patterns in one pass over the text. Patterns only report whether
// they matched, not where, and capturing groups count as plain groups.
//
// Rules that look at differently transformed text cannot share a set: see
// feature/regex.md section 9.
class RegexSet {
public:
    class Builder {
    public:
        explicit Builder(const Options &options = {});
        ~Builder();
        Builder(Builder &&) noexcept;
        Builder &operator=(Builder &&) noexcept;

        // Returns the id of the pattern, which is what MatchSet reports.
        [[nodiscard]] std::expected<std::uint32_t, Error> add(std::string_view pattern);
        // A set may hold 16 times the instructions a single Regex is allowed.
        [[nodiscard]] std::expected<RegexSet, Error> build() &&;

    private:
        std::unique_ptr<detail::SetBuilderState> state_;
    };

    [[nodiscard]] std::uint32_t size() const noexcept;
    [[nodiscard]] MatchSet make_match_set() const { return MatchSet(size()); }

    // Adds the patterns that match `text` to `hits`. Returns Match if there are any.
    [[nodiscard]] MatchStatus scan(std::string_view text, MatchSet &hits, Scratch &scratch) const noexcept;
    [[nodiscard]] MatchStatus scan(std::string_view text, MatchSet &hits, Scratch &scratch,
                                   WorkBudget &budget) const noexcept;

private:
    friend class Scanner;

    explicit RegexSet(std::shared_ptr<const detail::CompiledSet> compiled) noexcept;

    std::shared_ptr<const detail::CompiledSet> compiled_;
};

// Scans text that arrives in pieces, such as a request body. Belongs to one stream
// and may live across co_await; it keeps the program alive. Matching a single
// pattern this way takes a RegexSet of one.
//
// feed() is synchronous, so a caller on an event loop should keep chunks small and
// yield between them: see feature/regex.md section 8.4.
class Scanner {
public:
    explicit Scanner(const RegexSet &set);
    ~Scanner();
    Scanner(Scanner &&) noexcept;
    Scanner &operator=(Scanner &&) noexcept;

    // Returns Match once any pattern has matched, which stays true from then on.
    [[nodiscard]] MatchStatus feed(std::string_view chunk, Scratch &scratch, WorkBudget &budget) noexcept;
    // Must be called once at the end: matches that depend on the end of the text,
    // like those of a$, are only known then.
    [[nodiscard]] MatchStatus finish(Scratch &scratch, WorkBudget &budget) noexcept;

    [[nodiscard]] const MatchSet &hits() const noexcept;

private:
    std::unique_ptr<detail::ScannerState> state_;
};

} // namespace fiber::regex

#endif // FIBER_REGEX_REGEX_SET_H
