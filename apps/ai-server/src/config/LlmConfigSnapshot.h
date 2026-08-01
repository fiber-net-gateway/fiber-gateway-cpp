#ifndef FIBER_AI_SERVER_LLM_CONFIG_SNAPSHOT_H
#define FIBER_AI_SERVER_LLM_CONFIG_SNAPSHOT_H

#include "../discovery/WeightedRendezvous.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fiber::ai_server {

inline constexpr std::string_view kLlmConfigGroup = "LLM-SERVER";
inline constexpr std::string_view kBt1KeysDataId = "ploto.ai-llm.auth.bt1.keys";
inline constexpr std::string_view kModelsDataId = "ploto.ai-llm.models";
inline constexpr std::string_view kProviderDataIdPrefix = "ploto.ai-llm.provider.";
inline constexpr std::string_view kUserGroupDataIdPrefix = "ploto.ai-llm.user-group.";
inline constexpr std::string_view kDefaultNamingGroup = "DEFAULT_GROUP";

struct ConfigMetadata {
    std::string data_id;
    std::string group;
    std::string md5;
    std::int32_t version = 0;
};

struct Bt1Key {
    std::string kid;
    std::string secret;
};

struct Bt1KeySnapshot {
    ConfigMetadata metadata;
    std::int32_t clock_skew_seconds = 0;
    std::vector<Bt1Key> keys;

    [[nodiscard]] const Bt1Key *find_key(std::string_view kid) const noexcept;
};

struct UserGroupSnapshot {
    ConfigMetadata metadata;
    std::string name;
    std::vector<std::string> users;

    [[nodiscard]] bool contains(std::string_view user) const noexcept;
};

enum class ProviderProtocolType : std::uint8_t {
    OpenAiChatCompletions,
    OpenAiEmbedding,
    AnthropicMessages,
};

struct ProviderApiToken {
    std::string name;
    std::string token;
};

struct ProviderProtocol {
    ProviderProtocolType type = ProviderProtocolType::OpenAiChatCompletions;
    std::string path;
    std::string model;
};

struct ProviderConfigSnapshot {
    ConfigMetadata metadata;
    std::string name;
    std::string base_url;
    std::vector<ProviderApiToken> api_tokens;
    std::vector<ProviderProtocol> protocols;

    [[nodiscard]] const ProviderProtocol *find_protocol(ProviderProtocolType type) const noexcept;
};

struct LoadBalanceConfig {
    static constexpr std::int32_t kDefaultPrefixMaxBytes = 2048;

    std::int32_t prefix_max_bytes = kDefaultPrefixMaxBytes;
    std::int32_t max_primary_attempts = 0;
    bool fallback_enabled = true;
    std::vector<std::int32_t> retryable_statuses{429, 502, 503, 504};

    [[nodiscard]] bool is_retryable_status(std::int32_t status) const noexcept;
};

struct ModelRateLimitConfig {
    std::int64_t window_duration_millis = 0;
    std::int64_t max_tokens_per_window = 0;
};

struct CompiledModelRateLimitRule {
    std::int64_t revision = 0;
    std::int64_t window_duration_millis = 0;
    std::int64_t max_tokens_per_window = 0;
};

struct ModelDefinition {
    std::string model_name;
    std::vector<std::string> providers;
    std::optional<std::string> fallback_provider;
    std::vector<std::string> allow_user_groups;
    LoadBalanceConfig load_balance;
    std::optional<ModelRateLimitConfig> rate_limit;
};

struct ModelsConfigSnapshot {
    ConfigMetadata metadata;
    std::vector<ModelDefinition> models;
};

struct ProjectProvider {
    std::string name;
    std::shared_ptr<const ProviderConfigSnapshot> config;
    std::shared_ptr<WeightedRendezvous> service;
};

struct CompiledModelRoute {
    std::string model_name;
    std::vector<std::shared_ptr<const ProjectProvider>> providers;
    std::shared_ptr<const ProjectProvider> fallback_provider;
    std::vector<std::shared_ptr<const UserGroupSnapshot>> allow_user_groups;
    LoadBalanceConfig load_balance;
    std::optional<CompiledModelRateLimitRule> rate_limit;
};

class LlmProjectSnapshot {
public:
    LlmProjectSnapshot(ConfigMetadata metadata, std::uint64_t generation,
                       std::vector<std::shared_ptr<const ProjectProvider>> providers,
                       std::vector<CompiledModelRoute> models) noexcept;

    [[nodiscard]] const ConfigMetadata &metadata() const noexcept { return metadata_; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] const std::vector<std::shared_ptr<const ProjectProvider>> &providers() const noexcept {
        return providers_;
    }
    [[nodiscard]] const std::vector<CompiledModelRoute> &models() const noexcept { return models_; }

    [[nodiscard]] const ProjectProvider *find_provider(std::string_view name) const noexcept;
    [[nodiscard]] const CompiledModelRoute *find_model(std::string_view name) const noexcept;

private:
    ConfigMetadata metadata_;
    std::uint64_t generation_ = 0;
    std::vector<std::shared_ptr<const ProjectProvider>> providers_;
    std::vector<CompiledModelRoute> models_;
};

struct LlmConfigSnapshot {
    std::uint64_t generation = 0;
    std::shared_ptr<const Bt1KeySnapshot> bt1_keys;
    std::shared_ptr<const LlmProjectSnapshot> project;
};

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_LLM_CONFIG_SNAPSHOT_H
