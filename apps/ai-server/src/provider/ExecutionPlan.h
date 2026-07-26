#ifndef FIBER_AI_SERVER_EXECUTION_PLAN_H
#define FIBER_AI_SERVER_EXECUTION_PLAN_H

#include "../protocol/LlmBody.h"
#include "../routing/ModelAuthorization.h"
#include "ProviderRuntime.h"

#include <chrono>
#include <cstdint>
#include <expected>
#include <string_view>

#include <common/json/JsonValue.h>
#include <common/mem/BufPool.h>

namespace fiber::ai_server {

enum class ExecutionPlanErrorCode : std::uint8_t {
    ProviderConfigUnavailable,
    ProviderTokenUnavailable,
    ProviderProtocolUnsupported,
    OutOfMemory,
};

struct ExecutionPlanError {
    ExecutionPlanErrorCode code = ExecutionPlanErrorCode::ProviderConfigUnavailable;
    const char *message = nullptr;
};

struct ResolvedProviderAttempt {
    const ProjectProvider *provider = nullptr;
    const ProviderProtocol *protocol = nullptr;
    const ProviderApiToken *api_token = nullptr;
    ProviderRuntimeState *runtime = nullptr;
    bool fallback = false;
};

struct ResolvedExecutionPlan {
    LlmWireProtocol client_protocol = LlmWireProtocol::OpenAiChatCompletions;
    std::string_view route_key;
    json::JsonArray<ResolvedProviderAttempt> attempts;
    LoadBalanceConfig load_balance;
};

[[nodiscard]] std::expected<ResolvedExecutionPlan, ExecutionPlanError>
resolve_execution_plan(const AuthorizedModel &model, LlmWireProtocol protocol, std::string_view route_key,
                       ProviderRuntimeRegistry &runtime_registry, ProviderRuntimeState::TimePoint now,
                       mem::BufPool &pool) noexcept;

[[nodiscard]] std::uint64_t rendezvous_score(std::string_view route_key, std::string_view candidate_key) noexcept;

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_EXECUTION_PLAN_H
