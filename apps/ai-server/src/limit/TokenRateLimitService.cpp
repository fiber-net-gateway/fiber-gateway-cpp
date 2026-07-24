#include "TokenRateLimitService.h"

#include "RateLimitHash.h"

#include <algorithm>
#include <new>

#include <common/Assert.h>

namespace fiber::ai_server {

TokenRateLimitService::TokenRateLimitService(std::size_t shard_count) {
    FIBER_ASSERT(shard_count > 0);
    shards_.reserve(shard_count);
    for (std::size_t i = 0; i < shard_count; ++i) {
        shards_.push_back(std::make_unique<Shard>());
    }
}

void TokenRateLimitService::update_project(std::shared_ptr<const LlmProjectSnapshot> project) noexcept {
    std::lock_guard project_guard(project_mutex_);
    if (project_ && project && project->generation() < project_->generation()) {
        return;
    }
    if (project_ == project) {
        return;
    }
    project_ = std::move(project);
    for (const auto &shard: shards_) {
        std::lock_guard shard_guard(shard->mutex);
        shard->manager.update_project(project_);
    }
}

bool TokenRateLimitService::has_rule(std::string_view model_name) const noexcept {
    std::lock_guard guard(project_mutex_);
    if (!project_) {
        return false;
    }
    const CompiledModelRoute *model = project_->find_model(model_name);
    return model && model->rate_limit.has_value();
}

TokenRateLimitService::Shard &TokenRateLimitService::shard_for(std::string_view user_id,
                                                               std::string_view model_name) noexcept {
    const std::size_t index = static_cast<std::size_t>(rate_limit_key_hash64(user_id, model_name) % shards_.size());
    return *shards_[index];
}

const TokenRateLimitService::Shard &TokenRateLimitService::shard_for(std::string_view user_id,
                                                                     std::string_view model_name) const noexcept {
    const std::size_t index = static_cast<std::size_t>(rate_limit_key_hash64(user_id, model_name) % shards_.size());
    return *shards_[index];
}

TokenRateLimitCheckResult TokenRateLimitService::check(std::string_view user_id, std::string_view model_name,
                                                       std::int64_t now_millis) {
    Shard &shard = shard_for(user_id, model_name);
    std::lock_guard guard(shard.mutex);
    return shard.manager.check(user_id, model_name, now_millis);
}

TokenRateLimitSettleResult TokenRateLimitService::settle(std::string_view user_id, std::string_view model_name,
                                                         TokenRateLimitTicket ticket, std::int64_t tokens,
                                                         bool count_usage, std::int64_t now_millis) noexcept {
    Shard &shard = shard_for(user_id, model_name);
    std::lock_guard guard(shard.mutex);
    return shard.manager.settle(user_id, model_name, ticket, tokens, count_usage, now_millis);
}

std::size_t TokenRateLimitService::sweep_expired(std::int64_t now_millis) noexcept {
    std::size_t removed = 0;
    for (const auto &shard: shards_) {
        std::lock_guard guard(shard->mutex);
        removed += shard->manager.sweep_expired(now_millis);
    }
    return removed;
}

TokenRateLimiterStats TokenRateLimitService::stats() const noexcept {
    TokenRateLimiterStats result;
    for (const auto &shard: shards_) {
        std::lock_guard guard(shard->mutex);
        const TokenRateLimiterStats current = shard->manager.stats();
        result.limiter_count += current.limiter_count;
        result.in_flight_count += current.in_flight_count;
    }
    return result;
}

} // namespace fiber::ai_server
