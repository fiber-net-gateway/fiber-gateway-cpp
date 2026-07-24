#include "TokenRateLimitCoordinator.h"

#include <utility>

#include <async/Spawn.h>
#include <common/Assert.h>
#include <log/Log.h>

namespace fiber::ai_server {
namespace {

DEFINE_LOGGER(LOG_RATE_LIMIT, "ai_server.rate_limit");

RateLimitCoordinatorError coordinator_error(RateLimitCoordinatorErrorCode code,
                                            RateLimitRemoteError remote = {}) noexcept {
    return RateLimitCoordinatorError{.code = code, .remote = remote};
}

TokenRateLimitCheckResult from_http_response(const RateLimitCheckResponse &value) noexcept {
    TokenRateLimitCheckResult output{
            .rule_matched = value.rule_matched,
            .allowed = value.allowed,
            .used_tokens = value.used_tokens,
            .max_tokens = value.max_tokens,
            .recover_at_millis = value.recover_at_millis,
    };
    if (value.ticket) {
        output.has_ticket = true;
        output.ticket = TokenRateLimitTicket{
                .rule_revision = value.ticket->rule_revision,
                .generation = value.ticket->generation,
                .window_start_millis = value.ticket->window_start_millis,
        };
    }
    return output;
}

} // namespace

TokenRateLimitCoordinator::~TokenRateLimitCoordinator() {
    FIBER_ASSERT(!initialized_);
    FIBER_ASSERT(remote_settles_.empty());
}

bool TokenRateLimitCoordinator::init() noexcept {
    if (initialized_) {
        return true;
    }
    if (!remote_client_->init()) {
        return false;
    }
    stopping_.store(false, std::memory_order_release);
    initialized_ = true;
    return true;
}

async::Task<void> TokenRateLimitCoordinator::shutdown() noexcept {
    if (!initialized_) {
        co_return;
    }
    stopping_.store(true, std::memory_order_release);
    co_await remote_settles_.join();
    co_await remote_client_->shutdown();
    initialized_ = false;
}

async::Task<std::expected<CoordinatedRateLimitCheck, RateLimitCoordinatorError>>
TokenRateLimitCoordinator::check(std::string_view user_id, const CompiledModelRoute &model,
                                 std::int64_t now_millis) noexcept {
    if (!model.rate_limit) {
        co_return CoordinatedRateLimitCheck{
                .result = TokenRateLimitCheckResult{},
        };
    }
    const std::string_view model_name = model.model_name;
    const CompiledModelRateLimitRule &rule = *model.rate_limit;
    const auto owner = ring_->locate(user_id, model_name);
    if (!owner) {
        co_return std::unexpected(coordinator_error(RateLimitCoordinatorErrorCode::RingUnavailable));
    }

    TokenRateLimitCheckResult result;
    if (owner->local) {
        result = local_service_->check(user_id, model_name, rule, now_millis);
    } else {
        auto response =
                co_await remote_client_->check(*owner, RateLimitCheckRequest{
                                                               .user_id = user_id,
                                                               .model_name = model_name,
                                                               .rule_revision = rule.revision,
                                                               .window_duration_millis = rule.window_duration_millis,
                                                               .max_tokens_per_window = rule.max_tokens_per_window,
                                                       });
        if (!response) {
            co_return std::unexpected(
                    coordinator_error(RateLimitCoordinatorErrorCode::OwnerRequestFailed, response.error()));
        }
        result = from_http_response(*response);
    }
    if (!result.rule_matched) {
        co_return std::unexpected(coordinator_error(RateLimitCoordinatorErrorCode::OwnerRuleUnavailable));
    }
    if (result.allowed && (!result.has_ticket || result.ticket.rule_revision != rule.revision)) {
        co_return std::unexpected(coordinator_error(RateLimitCoordinatorErrorCode::InvalidOwnerResponse));
    }
    co_return CoordinatedRateLimitCheck{
            .result = result,
            .owner = *owner,
    };
}

void TokenRateLimitCoordinator::settle(RateLimitNode owner, std::string_view user_id, std::string_view model_name,
                                       TokenRateLimitTicket ticket, std::int64_t tokens, bool count_usage,
                                       std::int64_t now_millis, RateLimitSettleCompletion completion) noexcept {
    if (!initialized_ || stopping_.load(std::memory_order_acquire)) {
        LOG(LOG_RATE_LIMIT, WARN) << "token rate limit settle rejected while stopping owner="
                                  << log::quoted(owner.node_id);
        completion.notify(RateLimitSettleOutcome::Error);
        return;
    }
    if (owner.local) {
        const TokenRateLimitSettleResult result =
                local_service_->settle(user_id, model_name, ticket, tokens, count_usage, now_millis);
        if (!result.applied) {
            LOG(LOG_RATE_LIMIT, WARN) << "stale local token rate limit settle ignored owner="
                                      << log::quoted(owner.node_id);
        }
        completion.notify(result.applied ? RateLimitSettleOutcome::Applied : RateLimitSettleOutcome::Stale);
        return;
    }
    remote_settles_.add();
    async::spawn([this, owner = std::move(owner), user_id = std::string(user_id), model_name = std::string(model_name),
                  ticket, tokens, count_usage, completion]() mutable {
        return settle_remote(std::move(owner), std::move(user_id), std::move(model_name), ticket, tokens, count_usage,
                             completion);
    });
}

async::Task<std::expected<RateLimitSettleResponse, RateLimitCoordinatorError>>
TokenRateLimitCoordinator::settle_and_wait(RateLimitNode owner, std::string_view user_id, std::string_view model_name,
                                           TokenRateLimitTicket ticket, std::int64_t tokens, bool count_usage,
                                           std::int64_t now_millis) noexcept {
    if (!initialized_ || stopping_.load(std::memory_order_acquire)) {
        co_return std::unexpected(coordinator_error(RateLimitCoordinatorErrorCode::Stopping));
    }
    if (owner.local) {
        co_return to_http_response(
                local_service_->settle(user_id, model_name, ticket, tokens, count_usage, now_millis));
    }
    co_return co_await settle_remote_and_wait(owner, user_id, model_name, ticket, tokens, count_usage);
}

async::Task<std::expected<RateLimitSettleResponse, RateLimitCoordinatorError>>
TokenRateLimitCoordinator::settle_remote_and_wait(const RateLimitNode &owner, std::string_view user_id,
                                                  std::string_view model_name, TokenRateLimitTicket ticket,
                                                  std::int64_t tokens, bool count_usage) noexcept {
    FIBER_ASSERT(!owner.local);
    auto response = co_await remote_client_->settle(
            owner, RateLimitSettleRequest{
                           .user_id = user_id,
                           .model_name = model_name,
                           .ticket =
                                   RateLimitTicketPayload{
                                           .rule_revision = ticket.rule_revision,
                                           .generation = ticket.generation,
                                           .window_start_millis = ticket.window_start_millis,
                                   },
                           .tokens = tokens,
                           .count_usage = count_usage,
                   });
    if (!response) {
        co_return std::unexpected(
                coordinator_error(RateLimitCoordinatorErrorCode::OwnerRequestFailed, response.error()));
    }
    co_return *response;
}

async::DetachedTask TokenRateLimitCoordinator::settle_remote(RateLimitNode owner, std::string user_id,
                                                             std::string model_name, TokenRateLimitTicket ticket,
                                                             std::int64_t tokens, bool count_usage,
                                                             RateLimitSettleCompletion completion) noexcept {
    FIBER_ASSERT(!owner.local);
    auto result = co_await settle_remote_and_wait(owner, user_id, model_name, ticket, tokens, count_usage);
    RateLimitSettleOutcome outcome = RateLimitSettleOutcome::Applied;
    if (!result) {
        LOG(LOG_RATE_LIMIT, WARN) << "remote token rate limit settle failed owner=" << log::quoted(owner.node_id)
                                  << " error=" << static_cast<int>(result.error().code)
                                  << " io_error=" << common::io_err_name(result.error().remote.io_error);
        outcome = RateLimitSettleOutcome::Error;
    } else if (result->stale || !result->applied) {
        LOG(LOG_RATE_LIMIT, WARN) << "stale token rate limit settle ignored owner=" << log::quoted(owner.node_id);
        outcome = RateLimitSettleOutcome::Stale;
    }
    completion.notify(outcome);
    remote_settles_.done();
}

} // namespace fiber::ai_server
