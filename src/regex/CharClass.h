#ifndef FIBER_REGEX_CHAR_CLASS_H
#define FIBER_REGEX_CHAR_CLASS_H

#include <cstdint>
#include <vector>

#include "regex/Rune.h"

namespace fiber::regex::detail {

struct RuneRange {
    constexpr RuneRange() noexcept = default;
    constexpr RuneRange(Rune low, Rune high) noexcept : lo(low), hi(high) {}

    Rune lo = 0;
    Rune hi = 0;

    bool operator==(const RuneRange &) const = default;
};

// A set of runes kept as sorted, non-adjacent ranges.
class CharClass {
public:
    void add_range(Rune lo, Rune hi);
    // Adds lo-hi and, for ASCII letters, the other case. Folding is ASCII only in
    // both encodings: see feature/regex.md section 4.3.
    void add_folded_range(Rune lo, Rune hi);
    void add_class(const CharClass &other);

    void negate();
    void remove_above(Rune max);

    [[nodiscard]] bool contains(Rune r) const noexcept;
    [[nodiscard]] bool empty() const noexcept { return ranges_.empty(); }
    [[nodiscard]] std::uint32_t rune_count() const noexcept;
    // True when A-Z and a-z behave the same, so the compiler may fold them.
    [[nodiscard]] bool folds_ascii() const noexcept;

    [[nodiscard]] const std::vector<RuneRange> &ranges() const noexcept { return ranges_; }

    bool operator==(const CharClass &) const = default;

private:
    std::vector<RuneRange> ranges_{};
};

} // namespace fiber::regex::detail

#endif // FIBER_REGEX_CHAR_CLASS_H
