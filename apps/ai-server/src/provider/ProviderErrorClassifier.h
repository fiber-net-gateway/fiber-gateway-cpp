#ifndef FIBER_AI_SERVER_PROVIDER_ERROR_CLASSIFIER_H
#define FIBER_AI_SERVER_PROVIDER_ERROR_CLASSIFIER_H

#include "ExecutionPlan.h"

#include <chrono>
#include <cstdint>
#include <string_view>

namespace fiber::ai_server {

enum class ProviderErrorScope : std::uint8_t {
    None,
    ApiToken,
    Provider,
};

struct ProviderErrorDecision {
    ProviderErrorScope scope = ProviderErrorScope::None;
    bool retryable = false;
    std::chrono::milliseconds unavailable_ttl{0};
    std::string_view reason;
};

[[nodiscard]] ProviderErrorDecision classify_provider_response(LlmWireProtocol protocol, int status_code,
                                                               std::string_view retry_after, std::string_view body,
                                                               const LoadBalanceConfig &load_balance,
                                                               bool response_started, mem::BufPool &pool) noexcept;

[[nodiscard]] ProviderErrorDecision classify_provider_transport_error(bool response_started) noexcept;

void apply_provider_error(const ResolvedProviderAttempt &attempt, const ProviderErrorDecision &decision,
                          ProviderRuntimeState::TimePoint now) noexcept;

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_PROVIDER_ERROR_CLASSIFIER_H
