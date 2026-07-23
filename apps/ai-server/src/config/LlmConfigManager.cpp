#include "LlmConfigManager.h"

#include <iostream>
#include <string_view>
#include <utility>

#include <async/Spawn.h>
#include <async/WhenAny.h>
#include <common/Assert.h>

namespace fiber::ai_server {

struct LlmConfigManager::ProviderEntry {
    explicit ProviderEntry(std::string value) : name(std::move(value)) {
        stop_publisher = stop.acquire_publisher();
        FIBER_ASSERT(stop_publisher.has_value());
    }

    std::string name;
    std::shared_ptr<const ProviderConfigSnapshot> current;
    async::Watch<bool> stop{false};
    std::optional<async::Watch<bool>::Publisher> stop_publisher;
};

struct LlmConfigManager::GroupEntry {
    explicit GroupEntry(std::string value) : state(std::make_shared<UserGroupState>(std::move(value))) {
        stop_publisher = stop.acquire_publisher();
        FIBER_ASSERT(stop_publisher.has_value());
    }

    std::shared_ptr<UserGroupState> state;
    async::Watch<bool> stop{false};
    std::optional<async::Watch<bool>::Publisher> stop_publisher;
};

LlmConfigManager::LlmConfigManager(event::EventLoop &loop, nacos::ConfigService &config_service) :
    loop_(&loop), config_service_(&config_service) {
    stop_publisher_ = stop_.acquire_publisher();
    FIBER_ASSERT(stop_publisher_.has_value());
}

LlmConfigManager::~LlmConfigManager() {
    FIBER_ASSERT(state_ == LlmConfigManagerState::Created || state_ == LlmConfigManagerState::Stopped);
    FIBER_ASSERT(tasks_.empty());
}

std::expected<void, nacos::ConfigServiceError> LlmConfigManager::start() {
    FIBER_ASSERT(loop_->in_loop());
    if (state_ != LlmConfigManagerState::Created) {
        return std::unexpected(nacos::ConfigServiceError{
                .code = nacos::ConfigServiceErrorCode::InvalidArgument,
                .io_error = common::IoErr::Already,
                .message = "LLM config manager is already started",
        });
    }
    auto bt1 = config_service_->subscribe(kBt1KeysDataId, kLlmConfigGroup);
    if (!bt1) {
        return std::unexpected(std::move(bt1.error()));
    }
    auto models = config_service_->subscribe(kModelsDataId, kLlmConfigGroup);
    if (!models) {
        return std::unexpected(std::move(models.error()));
    }

    state_ = LlmConfigManagerState::Running;
    tasks_.add(2);
    async::spawn(*loop_,
                 [this, subscription = std::move(*bt1)]() mutable { return watch_bt1(std::move(subscription)); });
    async::spawn(*loop_,
                 [this, subscription = std::move(*models)]() mutable { return watch_models(std::move(subscription)); });
    return {};
}

async::Task<void> LlmConfigManager::shutdown() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (state_ == LlmConfigManagerState::Stopped) {
        co_return;
    }
    if (state_ == LlmConfigManagerState::Created) {
        state_ = LlmConfigManagerState::Stopped;
        co_return;
    }
    if (state_ == LlmConfigManagerState::Running) {
        state_ = LlmConfigManagerState::Stopping;
        stop_publisher_->publish(true);
        stop_all_dynamic();
    }
    co_await tasks_.join();
    providers_.clear();
    groups_.clear();
    state_ = LlmConfigManagerState::Stopped;
}

async::DetachedTask LlmConfigManager::watch_bt1(nacos::Subscription<nacos::ConfigData> subscription) noexcept {
    auto stop = stop_.subscribe();
    auto stop_snapshot = stop.current();
    auto &subscriber = subscription.subscriber();
    auto snapshot = subscriber.current();
    for (;;) {
        if (snapshot.value != nullptr) {
            if (snapshot.value->kind == nacos::ResultKind::Closed) {
                break;
            }
            if (snapshot.value->data) {
                apply_bt1(*snapshot.value->data);
            }
        }
        auto result = co_await async::when_any(
                [&subscriber, version = snapshot.version]() { return subscriber.next(version); },
                [&stop, version = stop_snapshot.version]() { return stop.next(version); });
        if (result.is<1>()) {
            std::move(result).get<1>();
            break;
        }
        snapshot = std::move(result).get<0>();
        stop_snapshot = stop.current();
    }
    task_done();
}

async::DetachedTask LlmConfigManager::watch_models(nacos::Subscription<nacos::ConfigData> subscription) noexcept {
    auto stop = stop_.subscribe();
    auto stop_snapshot = stop.current();
    auto &subscriber = subscription.subscriber();
    auto snapshot = subscriber.current();
    for (;;) {
        if (snapshot.value != nullptr) {
            if (snapshot.value->kind == nacos::ResultKind::Closed) {
                break;
            }
            if (snapshot.value->data) {
                apply_models(*snapshot.value->data);
            }
        }
        auto result = co_await async::when_any(
                [&subscriber, version = snapshot.version]() { return subscriber.next(version); },
                [&stop, version = stop_snapshot.version]() { return stop.next(version); });
        if (result.is<1>()) {
            std::move(result).get<1>();
            break;
        }
        snapshot = std::move(result).get<0>();
        stop_snapshot = stop.current();
    }
    task_done();
}

async::DetachedTask LlmConfigManager::watch_provider(std::shared_ptr<ProviderEntry> entry,
                                                     nacos::Subscription<nacos::ConfigData> subscription) noexcept {
    auto stop = entry->stop.subscribe();
    auto stop_snapshot = stop.current();
    auto &subscriber = subscription.subscriber();
    auto snapshot = subscriber.current();
    for (;;) {
        if (snapshot.value != nullptr) {
            if (snapshot.value->kind == nacos::ResultKind::Closed) {
                break;
            }
            if (snapshot.value->data) {
                apply_provider(entry, *snapshot.value->data);
            }
        }
        auto result = co_await async::when_any(
                [&subscriber, version = snapshot.version]() { return subscriber.next(version); },
                [&stop, version = stop_snapshot.version]() { return stop.next(version); });
        if (result.is<1>()) {
            std::move(result).get<1>();
            break;
        }
        snapshot = std::move(result).get<0>();
        stop_snapshot = stop.current();
    }
    task_done();
}

async::DetachedTask LlmConfigManager::watch_group(std::shared_ptr<GroupEntry> entry,
                                                  nacos::Subscription<nacos::ConfigData> subscription) noexcept {
    auto stop = entry->stop.subscribe();
    auto stop_snapshot = stop.current();
    auto &subscriber = subscription.subscriber();
    auto snapshot = subscriber.current();
    for (;;) {
        if (snapshot.value != nullptr) {
            if (snapshot.value->kind == nacos::ResultKind::Closed) {
                break;
            }
            if (snapshot.value->data) {
                apply_group(entry, *snapshot.value->data);
            }
        }
        auto result = co_await async::when_any(
                [&subscriber, version = snapshot.version]() { return subscriber.next(version); },
                [&stop, version = stop_snapshot.version]() { return stop.next(version); });
        if (result.is<1>()) {
            std::move(result).get<1>();
            break;
        }
        snapshot = std::move(result).get<0>();
        stop_snapshot = stop.current();
    }
    task_done();
}

void LlmConfigManager::apply_bt1(const nacos::ConfigData &data) {
    FIBER_ASSERT(loop_->in_loop());
    if (data.state == nacos::ConfigState::NotFound) {
        report_not_found(kBt1KeysDataId);
        return;
    }
    auto parsed = parse_bt1_key_config(data.content, data.md5);
    if (!parsed) {
        report_failure(kBt1KeysDataId, data.md5, std::move(parsed.error()));
        return;
    }
    bt1_keys_ = std::make_shared<const Bt1KeySnapshot>(std::move(*parsed));
    ++successful_updates_;
}

void LlmConfigManager::apply_models(const nacos::ConfigData &data) {
    FIBER_ASSERT(loop_->in_loop());
    if (data.state == nacos::ConfigState::NotFound) {
        report_not_found(kModelsDataId);
        return;
    }
    auto parsed = parse_models_config(data.content, data.md5);
    if (!parsed) {
        report_failure(kModelsDataId, data.md5, std::move(parsed.error()));
        return;
    }

    std::unordered_set<std::string> provider_names;
    std::unordered_set<std::string> group_names;
    auto reconciled = reconcile_dependencies(*parsed, provider_names, group_names);
    if (!reconciled) {
        report_failure(kModelsDataId, data.md5,
                       LlmConfigError{
                               .code = LlmConfigErrorCode::InvalidField,
                               .field = "dependencies",
                               .message = reconciled.error().message.empty()
                                                  ? "failed to subscribe to model dependencies"
                                                  : reconciled.error().message,
                       });
        return;
    }

    models_ = std::make_shared<const ModelsConfigSnapshot>(std::move(*parsed));
    rebuild_project();
    remove_unreferenced(provider_names, group_names);
    ++successful_updates_;
}

void LlmConfigManager::apply_provider(const std::shared_ptr<ProviderEntry> &entry, const nacos::ConfigData &data) {
    FIBER_ASSERT(loop_->in_loop());
    const auto active = providers_.find(entry->name);
    if (active == providers_.end() || active->second != entry) {
        return;
    }
    const std::string data_id = std::string(kProviderDataIdPrefix) + entry->name;
    if (data.state == nacos::ConfigState::NotFound) {
        report_not_found(data_id);
        return;
    }
    auto parsed = parse_provider_config(data.content, data.md5, entry->name);
    if (!parsed) {
        report_failure(data_id, data.md5, std::move(parsed.error()));
        return;
    }
    entry->current = std::make_shared<const ProviderConfigSnapshot>(std::move(*parsed));
    rebuild_project();
    ++successful_updates_;
}

void LlmConfigManager::apply_group(const std::shared_ptr<GroupEntry> &entry, const nacos::ConfigData &data) {
    FIBER_ASSERT(loop_->in_loop());
    const auto active = groups_.find(entry->state->name());
    if (active == groups_.end() || active->second != entry) {
        return;
    }
    const std::string data_id = std::string(kUserGroupDataIdPrefix) + entry->state->name();
    if (data.state == nacos::ConfigState::NotFound) {
        report_not_found(data_id);
        return;
    }
    auto parsed = parse_user_group_config(data.content, data.md5, entry->state->name());
    if (!parsed) {
        report_failure(data_id, data.md5, std::move(parsed.error()));
        return;
    }
    entry->state->publish(std::make_shared<const UserGroupSnapshot>(std::move(*parsed)));
    ++successful_updates_;
}

std::expected<void, nacos::ConfigServiceError>
LlmConfigManager::reconcile_dependencies(const ModelsConfigSnapshot &models,
                                         std::unordered_set<std::string> &provider_names,
                                         std::unordered_set<std::string> &group_names) {
    std::vector<std::string> added_providers;
    std::vector<std::string> added_groups;
    for (const ModelDefinition &model: models.models) {
        for (const std::string &name: model.providers) {
            provider_names.emplace(name);
        }
        if (model.fallback_provider) {
            provider_names.emplace(*model.fallback_provider);
        }
        for (const std::string &name: model.allow_user_groups) {
            group_names.emplace(name);
        }
    }

    for (const std::string &name: provider_names) {
        if (providers_.contains(name)) {
            continue;
        }
        auto result = add_provider(name);
        if (!result) {
            for (const std::string &added: added_providers) {
                auto entry = providers_.find(added);
                entry->second->stop_publisher->publish(true);
                providers_.erase(entry);
            }
            return std::unexpected(std::move(result.error()));
        }
        added_providers.push_back(name);
    }
    for (const std::string &name: group_names) {
        if (groups_.contains(name)) {
            continue;
        }
        auto result = add_group(name);
        if (!result) {
            for (const std::string &added: added_groups) {
                auto entry = groups_.find(added);
                entry->second->stop_publisher->publish(true);
                groups_.erase(entry);
            }
            for (const std::string &added: added_providers) {
                auto entry = providers_.find(added);
                entry->second->stop_publisher->publish(true);
                providers_.erase(entry);
            }
            return std::unexpected(std::move(result.error()));
        }
        added_groups.push_back(name);
    }
    return {};
}

std::expected<void, nacos::ConfigServiceError> LlmConfigManager::add_provider(std::string name) {
    auto subscription = config_service_->subscribe(std::string(kProviderDataIdPrefix) + name, kLlmConfigGroup);
    if (!subscription) {
        return std::unexpected(std::move(subscription.error()));
    }
    auto entry = std::make_shared<ProviderEntry>(std::move(name));
    providers_.emplace(entry->name, entry);
    tasks_.add();
    async::spawn(*loop_, [this, entry, subscription = std::move(*subscription)]() mutable {
        return watch_provider(entry, std::move(subscription));
    });
    return {};
}

std::expected<void, nacos::ConfigServiceError> LlmConfigManager::add_group(std::string name) {
    auto subscription = config_service_->subscribe(std::string(kUserGroupDataIdPrefix) + name, kLlmConfigGroup);
    if (!subscription) {
        return std::unexpected(std::move(subscription.error()));
    }
    auto entry = std::make_shared<GroupEntry>(std::move(name));
    groups_.emplace(entry->state->name(), entry);
    tasks_.add();
    async::spawn(*loop_, [this, entry, subscription = std::move(*subscription)]() mutable {
        return watch_group(entry, std::move(subscription));
    });
    return {};
}

void LlmConfigManager::remove_unreferenced(const std::unordered_set<std::string> &provider_names,
                                           const std::unordered_set<std::string> &group_names) {
    for (auto it = providers_.begin(); it != providers_.end();) {
        if (provider_names.contains(it->first)) {
            ++it;
            continue;
        }
        it->second->stop_publisher->publish(true);
        it = providers_.erase(it);
    }
    for (auto it = groups_.begin(); it != groups_.end();) {
        if (group_names.contains(it->first)) {
            ++it;
            continue;
        }
        it->second->stop_publisher->publish(true);
        it = groups_.erase(it);
    }
}

void LlmConfigManager::stop_all_dynamic() noexcept {
    for (auto &[name, entry]: providers_) {
        (void) name;
        entry->stop_publisher->publish(true);
    }
    for (auto &[name, entry]: groups_) {
        (void) name;
        entry->stop_publisher->publish(true);
    }
}

void LlmConfigManager::rebuild_project() {
    if (!models_) {
        return;
    }
    std::vector<ProjectProvider> providers;
    std::vector<CompiledModelRoute> models;

    auto provider_index = [&](const std::string &name) {
        for (std::size_t i = 0; i < providers.size(); ++i) {
            if (providers[i].name == name) {
                return i;
            }
        }
        const auto entry = providers_.find(name);
        FIBER_ASSERT(entry != providers_.end());
        providers.push_back(ProjectProvider{.name = name, .config = entry->second->current});
        return providers.size() - 1;
    };

    models.reserve(models_->models.size());
    for (const ModelDefinition &definition: models_->models) {
        CompiledModelRoute route;
        route.model_name = definition.model_name;
        route.provider_indices.reserve(definition.providers.size());
        for (const std::string &name: definition.providers) {
            route.provider_indices.push_back(provider_index(name));
        }
        if (definition.fallback_provider) {
            route.fallback_provider_index = provider_index(*definition.fallback_provider);
        }
        route.allow_user_groups.reserve(definition.allow_user_groups.size());
        for (const std::string &name: definition.allow_user_groups) {
            const auto entry = groups_.find(name);
            FIBER_ASSERT(entry != groups_.end());
            route.allow_user_groups.push_back(entry->second->state);
        }
        route.load_balance = definition.load_balance;
        route.rate_limit = definition.rate_limit;
        models.push_back(std::move(route));
    }
    project_ = std::make_shared<const LlmProjectSnapshot>(models_->metadata, ++project_generation_,
                                                          std::move(providers), std::move(models));
}

void LlmConfigManager::report_failure(std::string_view data_id, std::string_view md5, LlmConfigError error) {
    ++failed_updates_;
    std::cerr << "LLM config update rejected dataId=" << data_id << " md5=" << md5;
    if (!error.field.empty()) {
        std::cerr << " field=" << error.field;
    }
    if (error.offset != 0) {
        std::cerr << " offset=" << error.offset;
    }
    std::cerr << ": " << error.message << '\n';
    last_failure_ = LlmConfigFailure{
            .data_id = std::string(data_id),
            .md5 = std::string(md5),
            .error = std::move(error),
    };
}

void LlmConfigManager::report_not_found(std::string_view data_id) {
    report_failure(data_id, {},
                   LlmConfigError{
                           .code = LlmConfigErrorCode::InvalidEnvelope,
                           .field = "data",
                           .message = "configuration is not found; retaining the last valid snapshot",
                   });
}

void LlmConfigManager::task_done() noexcept { tasks_.done(); }

} // namespace fiber::ai_server
