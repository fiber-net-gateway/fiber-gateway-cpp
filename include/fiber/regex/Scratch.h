#ifndef FIBER_REGEX_SCRATCH_H
#define FIBER_REGEX_SCRATCH_H

#include <cstddef>
#include <memory>

#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"

namespace fiber::regex {

namespace detail {
struct ScratchAccess;
struct ScratchState;
} // namespace detail

// Mutable working memory for matching, one per worker thread. Not thread safe.
// Only used inside synchronous calls, so nothing in it outlives a co_await.
class Scratch : public common::NonCopyable, public common::NonMovable {
public:
    // Sized for the OWASP CRS rule sets, whose DFA states take 27 MiB when warm. A
    // budget below the working set costs far more than the memory it saves: see
    // feature/regex.md section 18.
    static constexpr std::size_t kDefaultDfaBudgetBytes = std::size_t{32} << 20U;

    explicit Scratch(std::size_t dfa_budget_bytes = kDefaultDfaBudgetBytes);
    ~Scratch();

    [[nodiscard]] std::size_t dfa_budget_bytes() const noexcept { return dfa_budget_bytes_; }
    // How much of the budget is in use, for sizing it against real traffic.
    [[nodiscard]] std::size_t dfa_bytes_used() const noexcept;

private:
    friend struct detail::ScratchAccess;

    std::size_t dfa_budget_bytes_;
    std::unique_ptr<detail::ScratchState> state_;
};

} // namespace fiber::regex

#endif // FIBER_REGEX_SCRATCH_H
