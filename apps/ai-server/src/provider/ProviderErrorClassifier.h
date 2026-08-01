#ifndef FIBER_AI_SERVER_PROVIDER_ERROR_CLASSIFIER_H
#define FIBER_AI_SERVER_PROVIDER_ERROR_CLASSIFIER_H

#include "../discovery/WeightedRendezvous.h"
#include "ExecutionPlan.h"
#include "ProviderHttpClient.h"

#include <chrono>
#include <cstdint>
#include <span>
#include <string_view>

namespace fiber::ai_server {

enum class ProviderErrorScope : std::uint8_t {
    None,
    ApiToken,
    Provider,
};

enum class ProviderRetryTarget : std::uint8_t {
    None,
    NextAttempt,
    NextProvider,
};

[[nodiscard]] std::string_view provider_retry_target_name(ProviderRetryTarget target) noexcept;

struct ProviderErrorDecision {
    ProviderErrorScope scope = ProviderErrorScope::None;
    InstanceReportOutcome instance_outcome = InstanceReportOutcome::Success;
    ProviderRetryTarget retry_target = ProviderRetryTarget::None;
    std::chrono::milliseconds unavailable_ttl{0};
    std::string_view reason;

    [[nodiscard]] bool retryable() const noexcept { return retry_target != ProviderRetryTarget::None; }
};

struct ProviderRetrySelection {
    ProviderRetryTarget retry_target = ProviderRetryTarget::None;
    std::size_t next_index = 0;
    std::size_t skipped_attempts = 0;
    bool retry_performed = false;
};

[[nodiscard]] ProviderErrorDecision classify_provider_response(LlmWireProtocol protocol, int status_code,
                                                               std::string_view retry_after, std::string_view body,
                                                               const LoadBalanceConfig &load_balance,
                                                               bool response_started, mem::BufPool &pool) noexcept;

[[nodiscard]] ProviderErrorDecision classify_provider_transport_error(const ProviderHttpError &error,
                                                                      bool response_started) noexcept;

[[nodiscard]] ProviderErrorDecision classify_provider_transport_error(ProviderHttpErrorCode code,
                                                                      bool response_started) noexcept;

[[nodiscard]] ProviderRetrySelection select_provider_retry(std::span<const ResolvedProviderAttempt> attempts,
                                                           std::size_t current_index,
                                                           const ProviderErrorDecision &decision, bool response_started,
                                                           ProviderRuntimeState::TimePoint now) noexcept;

void apply_provider_error(const ResolvedProviderAttempt &attempt, const ProviderErrorDecision &decision,
                          ProviderRuntimeState::TimePoint now) noexcept;

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_PROVIDER_ERROR_CLASSIFIER_H
