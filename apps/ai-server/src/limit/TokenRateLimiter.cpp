#include "TokenRateLimiter.h"

#include <algorithm>
#include <limits>
#include <tuple>
#include <utility>

#include <fiber/common/Assert.h>

namespace fiber::ai_server {
namespace {

std::int64_t safe_add(std::int64_t left, std::int64_t right) noexcept {
    if (right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return left + right;
}

std::int64_t ceil_scaled(std::int64_t value, std::int64_t numerator, std::int64_t denominator) noexcept {
    if (value <= 0) {
        return 0;
    }
    if (denominator <= 0 || numerator <= 0 || value > std::numeric_limits<std::int64_t>::max() / numerator) {
        return std::numeric_limits<std::int64_t>::max();
    }
    const std::int64_t product = value * numerator;
    const std::int64_t quotient = product / denominator;
    return product % denominator == 0 ? quotient : safe_add(quotient, 1);
}

std::uint64_t next_generation(std::uint64_t current) noexcept {
    return current == std::numeric_limits<std::uint64_t>::max() ? current : current + 1;
}

} // namespace

bool WindowTokenRateLimiter::init(std::int64_t window_duration_millis, std::int64_t max_tokens_per_window) noexcept {
    if (window_duration_millis <= 0 || max_tokens_per_window < 0) {
        return false;
    }
    window_duration_millis_ = window_duration_millis;
    max_tokens_per_window_ = max_tokens_per_window;
    current_window_start_millis_ = kUnsetTime;
    current_window_used_tokens_ = 0;
    recover_at_millis_ = kUnsetTime;
    return true;
}

void WindowTokenRateLimiter::ensure_window(std::int64_t now_millis) noexcept {
    if (current_window_start_millis_ == kUnsetTime || now_millis >= recover_at_millis_) {
        current_window_start_millis_ = now_millis;
        current_window_used_tokens_ = 0;
        recover_at_millis_ = safe_add(now_millis, window_duration_millis_);
    }
}

WindowTokenRateLimitCheck WindowTokenRateLimiter::check(std::int64_t now_millis) noexcept {
    ensure_window(now_millis);
    const bool allowed = current_window_used_tokens_ < max_tokens_per_window_;
    return WindowTokenRateLimitCheck{
            .allowed = allowed,
            .used_tokens = current_window_used_tokens_,
            .max_tokens = max_tokens_per_window_,
            .recover_at_millis = recover_at_millis_,
            .has_ticket = allowed,
            .window_start_millis = allowed ? current_window_start_millis_ : 0,
    };
}

std::int64_t WindowTokenRateLimiter::compute_recover_at(std::int64_t used_tokens) const noexcept {
    const std::int64_t window_end = safe_add(current_window_start_millis_, window_duration_millis_);
    if (used_tokens <= max_tokens_per_window_ || max_tokens_per_window_ == 0) {
        return window_end;
    }
    const std::int64_t excess = used_tokens - max_tokens_per_window_;
    return safe_add(window_end, ceil_scaled(excess, window_duration_millis_, max_tokens_per_window_));
}

bool WindowTokenRateLimiter::settle(std::int64_t window_start_millis, std::int64_t tokens) noexcept {
    if (tokens < 0 || current_window_start_millis_ == kUnsetTime ||
        window_start_millis != current_window_start_millis_) {
        return false;
    }
    current_window_used_tokens_ = safe_add(current_window_used_tokens_, tokens);
    recover_at_millis_ = compute_recover_at(current_window_used_tokens_);
    return true;
}

bool TokenRateLimiterManager::RateLimitKeyLess::operator()(const RateLimitKey &left,
                                                           const RateLimitKey &right) const noexcept {
    return std::tie(left.user_id, left.model_name, left.rule_revision) <
           std::tie(right.user_id, right.model_name, right.rule_revision);
}

bool TokenRateLimiterManager::RateLimitKeyLess::operator()(const RateLimitKey &left,
                                                           RateLimitKeyView right) const noexcept {
    return std::tuple<std::string_view, std::string_view, std::int64_t>(left.user_id, left.model_name,
                                                                        left.rule_revision) <
           std::tuple(right.user_id, right.model_name, right.rule_revision);
}

bool TokenRateLimiterManager::RateLimitKeyLess::operator()(RateLimitKeyView left,
                                                           const RateLimitKey &right) const noexcept {
    return std::tuple(left.user_id, left.model_name, left.rule_revision) <
           std::tuple<std::string_view, std::string_view, std::int64_t>(right.user_id, right.model_name,
                                                                        right.rule_revision);
}

bool TokenRateLimiterManager::rule_changed(const LimiterState &state,
                                           const CompiledModelRateLimitRule &rule) const noexcept {
    return state.window_duration_millis != rule.window_duration_millis ||
           state.max_tokens_per_window != rule.max_tokens_per_window;
}

TokenRateLimiterManager::LimiterState
TokenRateLimiterManager::create_state(const CompiledModelRateLimitRule &rule,
                                      const LimiterState *old_state) const noexcept {
    LimiterState state;
    FIBER_ASSERT(state.limiter.init(rule.window_duration_millis, rule.max_tokens_per_window));
    state.window_duration_millis = rule.window_duration_millis;
    state.max_tokens_per_window = rule.max_tokens_per_window;
    state.generation = next_generation(old_state ? old_state->generation : 0);
    return state;
}

TokenRateLimitCheckResult TokenRateLimiterManager::check(std::string_view user_id, std::string_view model_name,
                                                         const CompiledModelRateLimitRule &rule,
                                                         std::int64_t now_millis) {
    const RateLimitKeyView key{
            .user_id = user_id,
            .model_name = model_name,
            .rule_revision = rule.revision,
    };
    auto found = limiters_.find(key);
    if (found == limiters_.end()) {
        auto inserted = limiters_.emplace(
                RateLimitKey{
                        .user_id = std::string(user_id),
                        .model_name = std::string(model_name),
                        .rule_revision = rule.revision,
                },
                create_state(rule, nullptr));
        found = inserted.first;
    } else if (rule_changed(found->second, rule)) {
        found->second = create_state(rule, &found->second);
    }

    LimiterState &state = found->second;
    state.last_touched_millis = now_millis;
    const WindowTokenRateLimitCheck result = state.limiter.check(now_millis);
    TokenRateLimitCheckResult output{
            .rule_matched = true,
            .allowed = result.allowed,
            .used_tokens = result.used_tokens,
            .max_tokens = result.max_tokens,
            .recover_at_millis = result.recover_at_millis,
    };
    if (result.allowed) {
        ++state.in_flight_count;
        output.has_ticket = true;
        output.ticket = TokenRateLimitTicket{
                .rule_revision = rule.revision,
                .generation = state.generation,
                .window_start_millis = result.window_start_millis,
        };
    }
    return output;
}

TokenRateLimitSettleResult TokenRateLimiterManager::settle(std::string_view user_id, std::string_view model_name,
                                                           TokenRateLimitTicket ticket, std::int64_t tokens,
                                                           bool count_usage, std::int64_t now_millis) noexcept {
    if (tokens < 0 || (!count_usage && tokens != 0)) {
        return {};
    }
    const RateLimitKeyView key{
            .user_id = user_id,
            .model_name = model_name,
            .rule_revision = ticket.rule_revision,
    };
    const auto found = limiters_.find(key);
    if (found == limiters_.end() || found->second.generation != ticket.generation) {
        return {};
    }

    LimiterState &state = found->second;
    state.last_touched_millis = now_millis;
    if (state.in_flight_count > 0) {
        --state.in_flight_count;
    }
    const bool usage_counted = count_usage && state.limiter.settle(ticket.window_start_millis, tokens);
    return TokenRateLimitSettleResult{
            .applied = true,
            .usage_counted = usage_counted,
            .used_tokens = state.limiter.current_window_used_tokens(),
            .recover_at_millis = state.limiter.recover_at_millis(),
    };
}

std::size_t TokenRateLimiterManager::sweep_expired(std::int64_t now_millis) noexcept {
    std::size_t removed = 0;
    for (auto it = limiters_.begin(); it != limiters_.end();) {
        const LimiterState &state = it->second;
        const std::int64_t idle_expire = safe_add(state.last_touched_millis, idle_ttl_millis_);
        const std::int64_t expire_at = std::max(idle_expire, state.limiter.recover_at_millis());
        if (state.in_flight_count == 0 && now_millis >= expire_at) {
            it = limiters_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

TokenRateLimiterStats TokenRateLimiterManager::stats() const noexcept {
    TokenRateLimiterStats result{.limiter_count = limiters_.size()};
    for (const auto &[key, state]: limiters_) {
        (void) key;
        result.in_flight_count += state.in_flight_count;
    }
    return result;
}

} // namespace fiber::ai_server
