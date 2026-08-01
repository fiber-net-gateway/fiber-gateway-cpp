#include "ExecutionPlan.h"

#include "../discovery/WeightedRendezvous.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <type_traits>

#include <openssl/sha.h>

namespace fiber::ai_server {
namespace {

struct ScoredAttempt {
    ResolvedProviderAttempt attempt;
    std::uint64_t provider_score = 0;
    std::uint64_t token_score = 0;
};

static_assert(std::is_trivially_copyable_v<ResolvedProviderAttempt>);
static_assert(std::is_trivially_copyable_v<ScoredAttempt>);

struct CandidateGroup {
    ScoredAttempt *attempts = nullptr;
    std::size_t size = 0;
    bool unavailable = false;
    bool token_unavailable = false;
};

ProviderProtocolType provider_protocol_type(LlmWireProtocol protocol) noexcept {
    return protocol == LlmWireProtocol::OpenAiChatCompletions ? ProviderProtocolType::OpenAiChatCompletions
                                                              : ProviderProtocolType::AnthropicMessages;
}

bool service_ready(const ProjectProvider &provider) noexcept {
    return !provider.config->base_url.starts_with("service://") ||
           (provider.service && provider.service->configured_instance_count() != 0);
}

std::size_t attempt_capacity(std::span<const std::shared_ptr<const ProjectProvider>> providers) noexcept {
    std::size_t capacity = 0;
    for (const auto &provider: providers) {
        if (!provider || !provider->config) {
            continue;
        }
        const std::size_t count = std::max<std::size_t>(provider->config->api_tokens.size(), 1);
        if (count > std::numeric_limits<std::size_t>::max() - capacity) {
            return 0;
        }
        capacity += count;
    }
    return capacity;
}

std::uint64_t token_score(std::string_view route_key, std::string_view provider_name,
                          std::string_view token_name) noexcept {
    if (route_key.empty() || provider_name.empty() || token_name.empty()) {
        return 0;
    }
    SHA256_CTX context;
    std::array<std::uint8_t, SHA256_DIGEST_LENGTH> digest{};
    if (SHA256_Init(&context) != 1 || SHA256_Update(&context, route_key.data(), route_key.size()) != 1 ||
        SHA256_Update(&context, "\n", 1) != 1 ||
        SHA256_Update(&context, provider_name.data(), provider_name.size()) != 1 ||
        SHA256_Update(&context, "\n", 1) != 1 || SHA256_Update(&context, token_name.data(), token_name.size()) != 1 ||
        SHA256_Final(digest.data(), &context) != 1) {
        return 0;
    }
    std::uint64_t score = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        score = (score << 8) | digest[i];
    }
    return score;
}

CandidateGroup build_candidates(std::span<const std::shared_ptr<const ProjectProvider>> providers,
                                ProviderProtocolType protocol_type, std::string_view route_key,
                                ProviderRuntimeRegistry &registry, ProviderRuntimeState::TimePoint now,
                                mem::BufPool &pool) noexcept {
    CandidateGroup group;
    const std::size_t capacity = attempt_capacity(providers);
    if (capacity == 0) {
        return group;
    }
    group.attempts = pool.alloc<ScoredAttempt>(capacity);
    if (!group.attempts) {
        group.size = std::numeric_limits<std::size_t>::max();
        return group;
    }

    for (const auto &provider_handle: providers) {
        if (!provider_handle || !provider_handle->config) {
            group.unavailable = true;
            continue;
        }
        const ProjectProvider &provider = *provider_handle;
        ProviderRuntimeState &runtime = registry.state_for(provider.name);
        if (!service_ready(provider) || (!provider.service && !runtime.available(now))) {
            group.unavailable = true;
            continue;
        }
        const ProviderProtocol *provider_protocol = provider.config->find_protocol(protocol_type);
        if (!provider_protocol) {
            continue;
        }

        const std::uint64_t provider_score = rendezvous_score(route_key, provider.name);
        if (provider.config->api_tokens.empty()) {
            group.attempts[group.size++] = ScoredAttempt{
                    .attempt =
                            ResolvedProviderAttempt{
                                    .provider = &provider,
                                    .protocol = provider_protocol,
                                    .runtime = &runtime,
                            },
                    .provider_score = provider_score,
            };
            continue;
        }

        std::size_t available_tokens = 0;
        for (const ProviderApiToken &token: provider.config->api_tokens) {
            if (token.token.empty() || !runtime.token_available(token.name, now)) {
                continue;
            }
            group.attempts[group.size++] = ScoredAttempt{
                    .attempt =
                            ResolvedProviderAttempt{
                                    .provider = &provider,
                                    .protocol = provider_protocol,
                                    .api_token = &token,
                                    .runtime = &runtime,
                            },
                    .provider_score = provider_score,
                    .token_score = token_score(route_key, provider.name, token.name),
            };
            ++available_tokens;
        }
        if (available_tokens == 0) {
            group.token_unavailable = true;
        }
    }

    if (!route_key.empty()) {
        std::sort(group.attempts, group.attempts + group.size,
                  [](const ScoredAttempt &left, const ScoredAttempt &right) {
                      if (left.provider_score != right.provider_score) {
                          return left.provider_score > right.provider_score;
                      }
                      if (left.attempt.provider->name != right.attempt.provider->name) {
                          return left.attempt.provider->name < right.attempt.provider->name;
                      }
                      if (left.token_score != right.token_score) {
                          return left.token_score > right.token_score;
                      }
                      const std::string_view left_name = left.attempt.api_token
                                                                 ? std::string_view(left.attempt.api_token->name)
                                                                 : std::string_view{};
                      const std::string_view right_name = right.attempt.api_token
                                                                  ? std::string_view(right.attempt.api_token->name)
                                                                  : std::string_view{};
                      return left_name < right_name;
                  });
    }
    return group;
}

} // namespace

std::uint64_t rendezvous_score(std::string_view route_key, std::string_view candidate_key) noexcept {
    if (route_key.empty() || candidate_key.empty()) {
        return 0;
    }
    SHA256_CTX context;
    std::array<std::uint8_t, SHA256_DIGEST_LENGTH> digest{};
    if (SHA256_Init(&context) != 1 || SHA256_Update(&context, route_key.data(), route_key.size()) != 1 ||
        SHA256_Update(&context, "\n", 1) != 1 ||
        SHA256_Update(&context, candidate_key.data(), candidate_key.size()) != 1 ||
        SHA256_Final(digest.data(), &context) != 1) {
        return 0;
    }
    std::uint64_t score = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        score = (score << 8) | digest[i];
    }
    return score;
}

std::expected<ResolvedExecutionPlan, ExecutionPlanError>
resolve_execution_plan(const AuthorizedModel &model, LlmWireProtocol protocol, std::string_view route_key,
                       ProviderRuntimeRegistry &runtime_registry, ProviderRuntimeState::TimePoint now,
                       mem::BufPool &pool) noexcept {
    if (!model.route) {
        return std::unexpected(ExecutionPlanError{
                .code = ExecutionPlanErrorCode::ProviderConfigUnavailable,
                .message = "authorized model is unavailable",
        });
    }
    const ProviderProtocolType provider_protocol = provider_protocol_type(protocol);
    CandidateGroup primary =
            build_candidates(model.route->providers, provider_protocol, route_key, runtime_registry, now, pool);
    std::array<std::shared_ptr<const ProjectProvider>, 1> fallback_source;
    std::size_t fallback_count = 0;
    if (model.route->fallback_provider) {
        fallback_source[0] = model.route->fallback_provider;
        fallback_count = 1;
    }
    CandidateGroup fallback = build_candidates(std::span(fallback_source.data(), fallback_count), provider_protocol,
                                               route_key, runtime_registry, now, pool);
    if (primary.size == std::numeric_limits<std::size_t>::max() ||
        fallback.size == std::numeric_limits<std::size_t>::max()) {
        return std::unexpected(ExecutionPlanError{
                .code = ExecutionPlanErrorCode::OutOfMemory,
                .message = "failed to allocate provider execution plan",
        });
    }

    std::size_t selected_primary = primary.size;
    const std::int32_t max_providers = model.route->load_balance.max_primary_attempts;
    if (max_providers > 0 && selected_primary > 0) {
        std::size_t provider_count = 0;
        std::string_view previous;
        selected_primary = 0;
        for (std::size_t i = 0; i < primary.size; ++i) {
            const std::string_view name = primary.attempts[i].attempt.provider->name;
            if (i == 0 || name != previous) {
                if (++provider_count > static_cast<std::size_t>(max_providers)) {
                    break;
                }
                previous = name;
            }
            ++selected_primary;
        }
    }
    const std::size_t selected_fallback = model.route->load_balance.fallback_enabled ? fallback.size : 0;
    const std::size_t total = selected_primary + selected_fallback;
    if (total == 0) {
        const bool fallback_enabled = model.route->load_balance.fallback_enabled;
        if (primary.unavailable || (fallback_enabled && fallback.unavailable)) {
            return std::unexpected(ExecutionPlanError{
                    .code = ExecutionPlanErrorCode::ProviderConfigUnavailable,
                    .message = "provider config is unavailable",
            });
        }
        if (primary.token_unavailable || (fallback_enabled && fallback.token_unavailable)) {
            return std::unexpected(ExecutionPlanError{
                    .code = ExecutionPlanErrorCode::ProviderTokenUnavailable,
                    .message = "provider api token is unavailable",
            });
        }
        return std::unexpected(ExecutionPlanError{
                .code = ExecutionPlanErrorCode::ProviderProtocolUnsupported,
                .message = "model provider does not support the client protocol",
        });
    }

    auto *attempts = pool.alloc<ResolvedProviderAttempt>(total);
    if (!attempts) {
        return std::unexpected(ExecutionPlanError{
                .code = ExecutionPlanErrorCode::OutOfMemory,
                .message = "failed to allocate provider execution plan",
        });
    }
    std::size_t output = 0;
    for (std::size_t i = 0; i < selected_primary; ++i) {
        attempts[output++] = std::move(primary.attempts[i].attempt);
    }
    for (std::size_t i = 0; i < selected_fallback; ++i) {
        fallback.attempts[i].attempt.fallback = true;
        attempts[output++] = std::move(fallback.attempts[i].attempt);
    }
    return ResolvedExecutionPlan{
            .client_protocol = protocol,
            .route_key = route_key,
            .attempts = json::JsonArray<ResolvedProviderAttempt>(attempts, output),
            .load_balance = model.route->load_balance,
    };
}

} // namespace fiber::ai_server
