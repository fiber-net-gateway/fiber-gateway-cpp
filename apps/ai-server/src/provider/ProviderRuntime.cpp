#include "ProviderRuntime.h"

#include <algorithm>
#include <utility>

namespace fiber::ai_server {
namespace {

constexpr std::size_t kProviderFailureThreshold = 3;
constexpr std::chrono::seconds kProviderOpenDuration{30};

} // namespace

ProviderRuntimeState::TokenState *ProviderRuntimeState::find_token(std::string_view name) noexcept {
    for (TokenState &token: tokens_) {
        if (token.name == name) {
            return &token;
        }
    }
    return nullptr;
}

void ProviderRuntimeState::clear_provider() noexcept {
    consecutive_failures_ = 0;
    unavailable_until_ = {};
}

bool ProviderRuntimeState::available(TimePoint now) noexcept {
    if (unavailable_until_ == TimePoint{} || unavailable_until_ > now) {
        return unavailable_until_ == TimePoint{};
    }
    clear_provider();
    return true;
}

bool ProviderRuntimeState::token_available(std::string_view token_name, TimePoint now) noexcept {
    if (!available(now)) {
        return false;
    }
    TokenState *state = find_token(token_name);
    if (!state) {
        return true;
    }
    if (state->unavailable_until <= now) {
        state->unavailable_until = {};
        return true;
    }
    return false;
}

void ProviderRuntimeState::record_success(std::string_view token_name) noexcept {
    clear_provider();
    if (TokenState *state = find_token(token_name)) {
        state->unavailable_until = {};
    }
}

void ProviderRuntimeState::record_provider_failure(TimePoint now) noexcept {
    if (unavailable_until_ > now) {
        return;
    }
    ++consecutive_failures_;
    if (consecutive_failures_ >= kProviderFailureThreshold) {
        consecutive_failures_ = 0;
        unavailable_until_ = now + kProviderOpenDuration;
    }
}

void ProviderRuntimeState::mark_provider_unavailable(TimePoint now, std::chrono::milliseconds ttl) noexcept {
    if (ttl <= std::chrono::milliseconds::zero()) {
        return;
    }
    consecutive_failures_ = 0;
    unavailable_until_ = std::max(unavailable_until_, now + ttl);
}

void ProviderRuntimeState::mark_token_unavailable(std::string_view token_name, TimePoint now,
                                                  std::chrono::milliseconds ttl) noexcept {
    if (token_name.empty() || ttl <= std::chrono::milliseconds::zero()) {
        return;
    }
    TokenState *state = find_token(token_name);
    if (!state) {
        tokens_.push_back(TokenState{.name = std::string(token_name)});
        state = &tokens_.back();
    }
    state->unavailable_until = std::max(state->unavailable_until, now + ttl);
}

void ProviderRuntimeState::retain_tokens(const std::vector<ProviderApiToken> &tokens) {
    std::erase_if(tokens_, [&tokens](const TokenState &state) {
        return std::none_of(tokens.begin(), tokens.end(),
                            [&state](const ProviderApiToken &token) { return token.name == state.name; });
    });
}

ProviderRuntimeState &ProviderRuntimeRegistry::state_for(std::string_view provider_name) {
    const auto found = states_.find(provider_name);
    if (found != states_.end()) {
        return *found->second;
    }
    auto state = std::make_unique<ProviderRuntimeState>();
    ProviderRuntimeState *result = state.get();
    states_.emplace(std::string(provider_name), std::move(state));
    return *result;
}

void ProviderRuntimeRegistry::reconcile(const LlmProjectSnapshot &project) {
    for (const auto &provider: project.providers()) {
        if (!provider || !provider->config) {
            continue;
        }
        state_for(provider->name).retain_tokens(provider->config->api_tokens);
    }
}

} // namespace fiber::ai_server
