#include "ProviderErrorClassifier.h"

#include <charconv>
#include <limits>

#include <fiber/common/Assert.h>
#include <fiber/common/json/JsonPath.h>

namespace fiber::ai_server {
namespace {

constexpr std::chrono::minutes kInvalidTokenTtl{5};
constexpr std::chrono::seconds kRateLimitTtl{30};

enum class ErrorFieldAction : std::uint32_t {
    Code,
    Type,
};

const json::JsonPathProgram &error_program() {
    static const json::JsonPathProgram program = [] {
        constexpr json::JsonPathRule rules[] = {
                {.expression = "$.error.code", .action = static_cast<std::uint32_t>(ErrorFieldAction::Code)},
                {.expression = "$.error.type", .action = static_cast<std::uint32_t>(ErrorFieldAction::Type)},
        };
        auto compiled = json::JsonPathProgram::compile(rules);
        FIBER_ASSERT(compiled.has_value());
        return std::move(*compiled);
    }();
    return program;
}

bool token_limit_name(LlmWireProtocol protocol, std::string_view value) noexcept {
    if (value == "rate_limit_error") {
        return true;
    }
    return protocol == LlmWireProtocol::OpenAiChatCompletions &&
           (value == "insufficient_quota" || value == "rate_limit_exceeded");
}

struct ErrorBodyContext {
    LlmWireProtocol protocol = LlmWireProtocol::OpenAiChatCompletions;
    bool token_error = false;
    std::string_view reason;
};

bool on_error_field(void *opaque, const json::JsonPathMatch &match) noexcept {
    auto &context = *static_cast<ErrorBodyContext *>(opaque);
    if (match.token.kind != json::TokenKind::Text || match.token.view.empty()) {
        return true;
    }
    if (context.reason.empty() || static_cast<ErrorFieldAction>(match.action) == ErrorFieldAction::Code) {
        context.reason = match.token.view;
    }
    if (token_limit_name(context.protocol, match.token.view)) {
        context.token_error = true;
    }
    return true;
}

std::chrono::milliseconds retry_after_ttl(std::string_view value, std::chrono::milliseconds fallback) noexcept {
    std::uint64_t seconds = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), seconds);
    if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || seconds == 0) {
        return fallback;
    }
    constexpr std::uint64_t kMaxMillis =
            static_cast<std::uint64_t>(std::numeric_limits<std::chrono::milliseconds::rep>::max());
    if (seconds > kMaxMillis / 1000) {
        return std::chrono::milliseconds::max();
    }
    return std::chrono::milliseconds(seconds * 1000);
}

} // namespace

std::string_view provider_retry_target_name(ProviderRetryTarget target) noexcept {
    switch (target) {
        case ProviderRetryTarget::None:
            return "none";
        case ProviderRetryTarget::NextAttempt:
            return "next_attempt";
        case ProviderRetryTarget::NextProvider:
            return "next_provider";
    }
    return "none";
}

ProviderErrorDecision classify_provider_response(LlmWireProtocol protocol, int status_code,
                                                 std::string_view retry_after, std::string_view body,
                                                 const LoadBalanceConfig &load_balance, bool response_started,
                                                 mem::BufPool &pool) noexcept {
    ErrorBodyContext body_context{.protocol = protocol};
    if (!body.empty()) {
        (void) json::visit_json_paths(error_program(), body, pool,
                                      json::JsonPathVisitor{
                                              .context = &body_context,
                                              .on_match = &on_error_field,
                                      });
    }

    const bool token_status = status_code == 401 || status_code == 403 || status_code == 429;
    if (token_status || body_context.token_error) {
        const std::chrono::milliseconds fallback =
                status_code == 401 || status_code == 403
                        ? std::chrono::duration_cast<std::chrono::milliseconds>(kInvalidTokenTtl)
                        : std::chrono::duration_cast<std::chrono::milliseconds>(kRateLimitTtl);
        return ProviderErrorDecision{
                .scope = ProviderErrorScope::ApiToken,
                .instance_outcome = InstanceReportOutcome::Neutral,
                .retry_target = response_started ? ProviderRetryTarget::None : ProviderRetryTarget::NextAttempt,
                .unavailable_ttl = retry_after_ttl(retry_after, fallback),
                .reason = !body_context.reason.empty() ? body_context.reason
                                                       : (status_code == 429 ? std::string_view("rate_limit")
                                                                             : std::string_view("auth_error")),
        };
    }

    const bool instance_failure = status_code == 502 || status_code == 503 || status_code == 504;
    const bool retryable = !response_started && load_balance.is_retryable_status(status_code);
    if (!retryable && !response_started) {
        return ProviderErrorDecision{
                .instance_outcome = instance_failure ? InstanceReportOutcome::Failure : InstanceReportOutcome::Success,
        };
    }
    return ProviderErrorDecision{
            .scope = ProviderErrorScope::Provider,
            .instance_outcome = instance_failure ? InstanceReportOutcome::Failure : InstanceReportOutcome::Success,
            .retry_target = retryable ? ProviderRetryTarget::NextAttempt : ProviderRetryTarget::None,
            .reason = "provider_status",
    };
}

ProviderErrorDecision classify_provider_transport_error(const ProviderHttpError &error,
                                                        bool response_started) noexcept {
    ProviderErrorDecision decision = classify_provider_transport_error(error.code, response_started);
    if (error.dns_backoff_hit) {
        decision.scope = ProviderErrorScope::None;
        decision.reason = "dns_backoff";
    }
    return decision;
}

ProviderErrorDecision classify_provider_transport_error(ProviderHttpErrorCode code, bool response_started) noexcept {
    return ProviderErrorDecision{
            .scope = ProviderErrorScope::Provider,
            .instance_outcome = InstanceReportOutcome::Failure,
            .retry_target = response_started ? ProviderRetryTarget::None
                                             : (code == ProviderHttpErrorCode::Dns ? ProviderRetryTarget::NextProvider
                                                                                   : ProviderRetryTarget::NextAttempt),
            .reason = code == ProviderHttpErrorCode::Dns ? std::string_view("provider_dns_error")
                                                         : std::string_view("provider_transport_error"),
    };
}

ProviderRetrySelection select_provider_retry(std::span<const ResolvedProviderAttempt> attempts,
                                             std::size_t current_index, const ProviderErrorDecision &decision,
                                             bool response_started, ProviderRuntimeState::TimePoint now) noexcept {
    if (response_started || current_index >= attempts.size()) {
        return {};
    }

    ProviderRetryTarget target = decision.retry_target;
    const ResolvedProviderAttempt &current = attempts[current_index];
    if (target == ProviderRetryTarget::NextAttempt && decision.scope == ProviderErrorScope::Provider &&
        current.provider && !current.provider->service && current.runtime &&
        current.runtime->provider_unavailable_until() > now) {
        target = ProviderRetryTarget::NextProvider;
    }
    if (target == ProviderRetryTarget::None) {
        return {};
    }
    if (target == ProviderRetryTarget::NextAttempt) {
        if (current_index + 1 >= attempts.size()) {
            return ProviderRetrySelection{.retry_target = target};
        }
        return ProviderRetrySelection{
                .retry_target = target,
                .next_index = current_index + 1,
                .retry_performed = true,
        };
    }

    std::size_t next = current_index + 1;
    while (next < attempts.size() && attempts[next].provider == current.provider) {
        ++next;
    }
    return ProviderRetrySelection{
            .retry_target = target,
            .next_index = next,
            .skipped_attempts = next - current_index - 1,
            .retry_performed = next < attempts.size(),
    };
}

void apply_provider_error(const ResolvedProviderAttempt &attempt, const ProviderErrorDecision &decision,
                          ProviderRuntimeState::TimePoint now) noexcept {
    if (!attempt.runtime) {
        return;
    }
    if (decision.scope == ProviderErrorScope::ApiToken) {
        if (attempt.api_token) {
            attempt.runtime->mark_token_unavailable(attempt.api_token->name, now, decision.unavailable_ttl);
        }
        return;
    }
    if (decision.scope == ProviderErrorScope::Provider) {
        if (attempt.provider && attempt.provider->service) {
            return;
        }
        if (decision.unavailable_ttl > std::chrono::milliseconds::zero()) {
            attempt.runtime->mark_provider_unavailable(now, decision.unavailable_ttl);
        } else {
            attempt.runtime->record_provider_failure(now);
        }
    }
}

} // namespace fiber::ai_server
