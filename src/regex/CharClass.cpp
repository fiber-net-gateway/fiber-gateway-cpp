#include "regex/CharClass.h"

#include <algorithm>

namespace fiber::regex::detail {

void CharClass::add_range(Rune lo, Rune hi) {
    if (hi < lo) {
        return;
    }
    // First existing range that overlaps or touches lo-hi.
    const auto first = std::lower_bound(ranges_.begin(), ranges_.end(), lo,
                                        [](const RuneRange &range, Rune value) { return range.hi + 1 < value; });
    auto last = first;
    while (last != ranges_.end() && last->lo <= hi + 1) {
        lo = std::min(lo, last->lo);
        hi = std::max(hi, last->hi);
        ++last;
    }
    if (first == last) {
        ranges_.insert(first, RuneRange{lo, hi});
        return;
    }
    *first = RuneRange{lo, hi};
    ranges_.erase(first + 1, last);
}

void CharClass::add_folded_range(Rune lo, Rune hi) {
    add_range(lo, hi);
    const Rune upper_lo = std::max<Rune>(lo, 'A');
    const Rune upper_hi = std::min<Rune>(hi, 'Z');
    if (upper_lo <= upper_hi) {
        add_range(upper_lo + ('a' - 'A'), upper_hi + ('a' - 'A'));
    }
    const Rune lower_lo = std::max<Rune>(lo, 'a');
    const Rune lower_hi = std::min<Rune>(hi, 'z');
    if (lower_lo <= lower_hi) {
        add_range(lower_lo - ('a' - 'A'), lower_hi - ('a' - 'A'));
    }
}

void CharClass::add_class(const CharClass &other) {
    for (const RuneRange &range: other.ranges_) {
        add_range(range.lo, range.hi);
    }
}

void CharClass::negate() {
    std::vector<RuneRange> negated;
    negated.reserve(ranges_.size() + 1);
    Rune next = 0;
    for (const RuneRange &range: ranges_) {
        if (next < range.lo) {
            negated.emplace_back(next, range.lo - 1);
        }
        next = range.hi + 1;
    }
    if (next <= kRuneMax) {
        negated.emplace_back(next, kRuneMax);
    }
    ranges_ = std::move(negated);
}

void CharClass::remove_above(Rune max) {
    while (!ranges_.empty() && ranges_.back().lo > max) {
        ranges_.pop_back();
    }
    if (!ranges_.empty() && ranges_.back().hi > max) {
        ranges_.back().hi = max;
    }
}

bool CharClass::contains(Rune r) const noexcept {
    const auto it = std::lower_bound(ranges_.begin(), ranges_.end(), r,
                                     [](const RuneRange &range, Rune value) { return range.hi < value; });
    return it != ranges_.end() && it->lo <= r;
}

std::uint32_t CharClass::rune_count() const noexcept {
    std::uint32_t count = 0;
    for (const RuneRange &range: ranges_) {
        count += static_cast<std::uint32_t>(range.hi - range.lo + 1);
    }
    return count;
}

bool CharClass::folds_ascii() const noexcept {
    for (Rune c = 'a'; c <= 'z'; ++c) {
        if (contains(c) != contains(c - ('a' - 'A'))) {
            return false;
        }
    }
    return true;
}

} // namespace fiber::regex::detail
