#include <fiber/regex/Scratch.h>

#include "regex/ScratchState.h"

namespace fiber::regex {

Scratch::Scratch(std::size_t dfa_budget_bytes) :
    dfa_budget_bytes_(dfa_budget_bytes), state_(std::make_unique<detail::ScratchState>(dfa_budget_bytes)) {}

Scratch::~Scratch() = default;

std::size_t Scratch::dfa_bytes_used() const noexcept { return state_->dfa.bytes_used(); }

} // namespace fiber::regex
