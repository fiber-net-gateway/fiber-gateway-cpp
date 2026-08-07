#ifndef FIBER_AI_SERVER_TOKEN_RATE_LIMITER_H
#define FIBER_AI_SERVER_TOKEN_RATE_LIMITER_H

#include "../config/LlmConfigSnapshot.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>

namespace fiber::ai_server {

struct TokenRateLimitTicket {
    std::int64_t rule_revision = 0;
    std::uint64_t generation = 0;
    std::int64_t window_start_millis = 0;
};

struct WindowTokenRateLimitCheck {
    bool allowed = false;
    std::int64_t used_tokens = 0;
    std::int64_t max_tokens = 0;
    std::int64_t recover_at_millis = 0;
    bool has_ticket = false;
    std::int64_t window_start_millis = 0;
};

class WindowTokenRateLimiter {
public:
    [[nodiscard]] bool init(std::int64_t window_duration_millis, std::int64_t max_tokens_per_window) noexcept;

    [[nodiscard]] WindowTokenRateLimitCheck check(std::int64_t now_millis) noexcept;
    [[nodiscard]] bool settle(std::int64_t window_start_millis, std::int64_t tokens) noexcept;

    [[nodiscard]] std::int64_t window_duration_millis() const noexcept { return window_duration_millis_; }
    [[nodiscard]] std::int64_t max_tokens_per_window() const noexcept { return max_tokens_per_window_; }
    [[nodiscard]] std::int64_t current_window_start_millis() const noexcept { return current_window_start_millis_; }
    [[nodiscard]] std::int64_t current_window_used_tokens() const noexcept { return current_window_used_tokens_; }
    [[nodiscard]] std::int64_t recover_at_millis() const noexcept { return recover_at_millis_; }

private:
    void ensure_window(std::int64_t now_millis) noexcept;
    [[nodiscard]] std::int64_t compute_recover_at(std::int64_t used_tokens) const noexcept;

    static constexpr std::int64_t kUnsetTime = INT64_MIN;

    std::int64_t window_duration_millis_ = 0;
    std::int64_t max_tokens_per_window_ = 0;
    std::int64_t current_window_start_millis_ = kUnsetTime;
    std::int64_t current_window_used_tokens_ = 0;
    std::int64_t recover_at_millis_ = kUnsetTime;
};

struct TokenRateLimitCheckResult {
    bool rule_matched = false;
    bool allowed = true;
    std::int64_t used_tokens = 0;
    std::int64_t max_tokens = 0;
    std::int64_t recover_at_millis = 0;
    bool has_ticket = false;
    TokenRateLimitTicket ticket;
};

struct TokenRateLimitSettleResult {
    bool applied = false;
    bool usage_counted = false;
    std::int64_t used_tokens = 0;
    std::int64_t recover_at_millis = 0;
};

struct TokenRateLimiterStats {
    std::size_t limiter_count = 0;
    std::size_t in_flight_count = 0;
};

class TokenRateLimiterManager final : public common::NonCopyable, public common::NonMovable {
public:
    explicit TokenRateLimiterManager(std::int64_t idle_ttl_millis = 10 * 60 * 1000) noexcept :
        idle_ttl_millis_(idle_ttl_millis) {}

    [[nodiscard]] TokenRateLimitCheckResult check(std::string_view user_id, std::string_view model_name,
                                                  const CompiledModelRateLimitRule &rule, std::int64_t now_millis);
    [[nodiscard]] TokenRateLimitSettleResult settle(std::string_view user_id, std::string_view model_name,
                                                    TokenRateLimitTicket ticket, std::int64_t tokens, bool count_usage,
                                                    std::int64_t now_millis) noexcept;

    [[nodiscard]] std::size_t sweep_expired(std::int64_t now_millis) noexcept;
    [[nodiscard]] TokenRateLimiterStats stats() const noexcept;
    void clear() noexcept { limiters_.clear(); }

private:
    struct RateLimitKey {
        std::string user_id;
        std::string model_name;
        std::int64_t rule_revision = 0;
    };

    struct RateLimitKeyView {
        std::string_view user_id;
        std::string_view model_name;
        std::int64_t rule_revision = 0;
    };

    struct RateLimitKeyLess {
        using is_transparent = void;

        bool operator()(const RateLimitKey &left, const RateLimitKey &right) const noexcept;
        bool operator()(const RateLimitKey &left, RateLimitKeyView right) const noexcept;
        bool operator()(RateLimitKeyView left, const RateLimitKey &right) const noexcept;
    };

    struct LimiterState {
        WindowTokenRateLimiter limiter;
        std::int64_t window_duration_millis = 0;
        std::int64_t max_tokens_per_window = 0;
        std::uint64_t generation = 0;
        std::int64_t last_touched_millis = 0;
        std::size_t in_flight_count = 0;
    };

    [[nodiscard]] bool rule_changed(const LimiterState &state, const CompiledModelRateLimitRule &rule) const noexcept;
    [[nodiscard]] LimiterState create_state(const CompiledModelRateLimitRule &rule,
                                            const LimiterState *old_state) const noexcept;

    std::map<RateLimitKey, LimiterState, RateLimitKeyLess> limiters_;
    std::int64_t idle_ttl_millis_ = 10 * 60 * 1000;
};

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_TOKEN_RATE_LIMITER_H
