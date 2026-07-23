#include "LlmConfigSnapshot.h"

#include <utility>

namespace fiber::ai_server {

const Bt1Key *Bt1KeySnapshot::find_key(std::string_view kid) const noexcept {
    const auto it = std::lower_bound(keys.begin(), keys.end(), kid,
                                     [](const Bt1Key &key, std::string_view name) { return key.kid < name; });
    return it != keys.end() && it->kid == kid ? &*it : nullptr;
}

bool UserGroupSnapshot::contains(std::string_view user) const noexcept {
    return std::binary_search(users.begin(), users.end(), user,
                              [](const auto &left, const auto &right) { return std::string_view(left) < right; });
}

UserGroupState::UserGroupState(std::string name) noexcept : name_(std::move(name)) {}

void UserGroupState::publish(std::shared_ptr<const UserGroupSnapshot> snapshot) noexcept {
    current_ = std::move(snapshot);
}

const ProviderProtocol *ProviderConfigSnapshot::find_protocol(ProviderProtocolType type) const noexcept {
    for (const ProviderProtocol &protocol: protocols) {
        if (protocol.type == type) {
            return &protocol;
        }
    }
    return nullptr;
}

bool LoadBalanceConfig::is_retryable_status(std::int32_t status) const noexcept {
    return std::binary_search(retryable_statuses.begin(), retryable_statuses.end(), status);
}

LlmProjectSnapshot::LlmProjectSnapshot(ConfigMetadata metadata, std::uint64_t generation,
                                       std::vector<ProjectProvider> providers,
                                       std::vector<CompiledModelRoute> models) noexcept :
    metadata_(std::move(metadata)), generation_(generation), providers_(std::move(providers)),
    models_(std::move(models)) {
    std::sort(models_.begin(), models_.end(), [](const CompiledModelRoute &left, const CompiledModelRoute &right) {
        return left.model_name < right.model_name;
    });
}

const ProjectProvider *LlmProjectSnapshot::find_provider(std::string_view name) const noexcept {
    for (const ProjectProvider &provider: providers_) {
        if (provider.name == name) {
            return &provider;
        }
    }
    return nullptr;
}

const CompiledModelRoute *LlmProjectSnapshot::find_model(std::string_view name) const noexcept {
    const auto it = std::lower_bound(
            models_.begin(), models_.end(), name,
            [](const CompiledModelRoute &model, std::string_view value) { return model.model_name < value; });
    return it != models_.end() && it->model_name == name ? &*it : nullptr;
}

} // namespace fiber::ai_server
