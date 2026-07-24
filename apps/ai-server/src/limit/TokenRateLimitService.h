#ifndef FIBER_AI_SERVER_TOKEN_RATE_LIMIT_SERVICE_H
#define FIBER_AI_SERVER_TOKEN_RATE_LIMIT_SERVICE_H

#include "TokenRateLimiter.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

#include <common/NonCopyable.h>
#include <common/NonMovable.h>

namespace fiber::ai_server {

class TokenRateLimitService final : public common::NonCopyable, public common::NonMovable {
public:
    explicit TokenRateLimitService(std::size_t shard_count);

    [[nodiscard]] TokenRateLimitCheckResult check(std::string_view user_id, std::string_view model_name,
                                                  const CompiledModelRateLimitRule &rule, std::int64_t now_millis);
    [[nodiscard]] TokenRateLimitSettleResult settle(std::string_view user_id, std::string_view model_name,
                                                    TokenRateLimitTicket ticket, std::int64_t tokens, bool count_usage,
                                                    std::int64_t now_millis) noexcept;

    [[nodiscard]] std::size_t sweep_expired(std::int64_t now_millis) noexcept;
    [[nodiscard]] TokenRateLimiterStats stats() const noexcept;

private:
    struct Shard {
        mutable std::mutex mutex;
        TokenRateLimiterManager manager;
    };

    [[nodiscard]] Shard &shard_for(std::string_view user_id, std::string_view model_name) noexcept;
    [[nodiscard]] const Shard &shard_for(std::string_view user_id, std::string_view model_name) const noexcept;

    std::vector<std::unique_ptr<Shard>> shards_;
};

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_TOKEN_RATE_LIMIT_SERVICE_H
