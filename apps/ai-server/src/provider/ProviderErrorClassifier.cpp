#include "ProviderErrorClassifier.h"

#include <charconv>
#include <limits>

#include <common/Assert.h>
#include <common/json/JsonPath.h>

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
                .retryable = !response_started,
                .unavailable_ttl = retry_after_ttl(retry_after, fallback),
                .reason = !body_context.reason.empty() ? body_context.reason
                                                       : (status_code == 429 ? std::string_view("rate_limit")
                                                                             : std::string_view("auth_error")),
        };
    }

    const bool retryable = !response_started && load_balance.is_retryable_status(status_code);
    if (!retryable && !response_started) {
        return {};
    }
    return ProviderErrorDecision{
            .scope = ProviderErrorScope::Provider,
            .retryable = retryable,
            .reason = "provider_status",
    };
}

ProviderErrorDecision classify_provider_transport_error(bool response_started) noexcept {
    return ProviderErrorDecision{
            .scope = ProviderErrorScope::Provider,
            .retryable = !response_started,
            .reason = "provider_transport_error",
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
        if (decision.unavailable_ttl > std::chrono::milliseconds::zero()) {
            attempt.runtime->mark_provider_unavailable(now, decision.unavailable_ttl);
        } else {
            attempt.runtime->record_provider_failure(now);
        }
    }
}

} // namespace fiber::ai_server
