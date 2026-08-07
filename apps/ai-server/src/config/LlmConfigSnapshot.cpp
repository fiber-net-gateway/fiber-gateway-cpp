#include "LlmConfigSnapshot.h"

#include <utility>

#include <fiber/common/Assert.h>

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
                                       std::vector<std::shared_ptr<const ProjectProvider>> providers,
                                       std::vector<CompiledModelRoute> models) noexcept :
    metadata_(std::move(metadata)), generation_(generation), providers_(std::move(providers)),
    models_(std::move(models)) {
    for (const auto &provider: providers_) {
        FIBER_ASSERT(provider != nullptr);
        FIBER_ASSERT(provider->config != nullptr);
        if (provider->config->base_url.starts_with("service://")) {
            FIBER_ASSERT(provider->service != nullptr);
        } else {
            FIBER_ASSERT(provider->service == nullptr);
        }
    }
    for (const CompiledModelRoute &model: models_) {
        for (const auto &provider: model.providers) {
            FIBER_ASSERT(provider != nullptr);
        }
        for (const auto &group: model.allow_user_groups) {
            FIBER_ASSERT(group != nullptr);
        }
    }
    std::sort(providers_.begin(), providers_.end(),
              [](const auto &left, const auto &right) { return left->name < right->name; });
    std::sort(models_.begin(), models_.end(), [](const CompiledModelRoute &left, const CompiledModelRoute &right) {
        return left.model_name < right.model_name;
    });
}

const ProjectProvider *LlmProjectSnapshot::find_provider(std::string_view name) const noexcept {
    const auto it = std::lower_bound(providers_.begin(), providers_.end(), name,
                                     [](const std::shared_ptr<const ProjectProvider> &provider,
                                        std::string_view value) { return provider->name < value; });
    return it != providers_.end() && (*it)->name == name ? it->get() : nullptr;
}

const CompiledModelRoute *LlmProjectSnapshot::find_model(std::string_view name) const noexcept {
    const auto it = std::lower_bound(
            models_.begin(), models_.end(), name,
            [](const CompiledModelRoute &model, std::string_view value) { return model.model_name < value; });
    return it != models_.end() && it->model_name == name ? &*it : nullptr;
}

} // namespace fiber::ai_server
