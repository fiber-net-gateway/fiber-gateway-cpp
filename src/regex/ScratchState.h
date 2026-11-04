#ifndef FIBER_REGEX_SCRATCH_STATE_H
#define FIBER_REGEX_SCRATCH_STATE_H

#include <cstddef>
#include <cstdint>

#include <fiber/regex/Scratch.h>

#include "regex/BitState.h"
#include "regex/Dfa.h"
#include "regex/PikeVm.h"

namespace fiber::regex::detail {

// Lets a test run one engine instead of the one the dispatcher would pick, which
// is how the engines are checked against each other. An engine that cannot
// handle a search still falls through to the Pike VM.
// Dfa makes the dispatcher use the DFA even where it would rather not bother.
enum class Engine : std::uint8_t { Auto, Pike, BitState, OnePass, Dfa };

struct ScratchState {
    explicit ScratchState(std::size_t dfa_budget_bytes) noexcept : dfa(dfa_budget_bytes) {}

    PikeWorkspace pike{};
    BitStateWorkspace bit_state{};
    DfaWorkspace dfa;
    Engine forced_engine = Engine::Auto;
};

struct ScratchAccess {
    [[nodiscard]] static ScratchState &state(Scratch &scratch) noexcept { return *scratch.state_; }
};

} // namespace fiber::regex::detail

#endif // FIBER_REGEX_SCRATCH_STATE_H
