#include "LlmConfigManager.h"

#include "../discovery/ServiceDiscovery.h"
#include "../limit/RateLimitHash.h"
#include "../observability/AiServerLogCategories.h"
#include "ConfigNodePool.h"

#include <algorithm>
#include <bit>
#include <functional>
#include <limits>
#include <map>
#include <string_view>
#include <utility>
#include <vector>

#include <async/Spawn.h>
#include <async/WaitGroup.h>
#include <async/WhenAny.h>
#include <common/Assert.h>
#include <log/Log.h>

namespace fiber::ai_server {
namespace {

DEFINE_LOGGER(LOG_CONFIG, kAiServerConfigLogger);
DEFINE_LOGGER(LOG_DISCOVERY, kAiServerDiscoveryLogger);

void log_config_rejection(bool serving_ready, std::string_view data_id, std::string_view md5,
                          const LlmConfigError &error) noexcept {
    const log::LogLevel level = serving_ready ? log::LogLevel::Warn : log::LogLevel::Error;
    const log::Logger &logger = LOG_CONFIG.get();
    if (!logger.enabled(level)) {
        return;
    }

    log::LogLine line(logger, level, __FILE__, __LINE__, __func__);
    line << "LLM config update rejected data_id=" << log::quoted(data_id) << " md5=" << log::quoted(md5);
    if (!error.field.empty()) {
        line << " field=" << log::quoted(error.field);
    }
    if (error.offset != 0) {
        line << " offset=" << error.offset;
    }
    line << " serving_ready=" << serving_ready << " reason=" << log::quoted(error.message);
}

class ConfigGraph;

class GroupNode final : public common::NonCopyable, public common::NonMovable {
public:
    using Key = std::string;
    using CreateError = nacos::ConfigServiceError;

    GroupNode(ConfigGraph &graph, Key key, nacos::Subscription<nacos::ConfigData> subscription);

    [[nodiscard]] const Key &key() const noexcept { return key_; }
    [[nodiscard]] const std::shared_ptr<const UserGroupSnapshot> &current() const noexcept { return current_; }

    void start(std::shared_ptr<GroupNode> self);
    void request_stop() noexcept;

private:
    [[nodiscard]] static async::DetachedTask run(std::shared_ptr<GroupNode> self) noexcept;
    void apply(const nacos::ConfigData &data);

    ConfigGraph *graph_ = nullptr;
    Key key_;
    nacos::Subscription<nacos::ConfigData> subscription_;
    std::shared_ptr<const UserGroupSnapshot> current_;
    async::Watch<bool> stop_{false};
    std::optional<async::Watch<bool>::Publisher> stop_publisher_;
    bool stopping_ = false;
};

using GroupNodePool = ConfigNodePool<GroupNode>;

class ProviderNode final : public common::NonCopyable, public common::NonMovable {
public:
    using Key = std::string;
    using CreateError = nacos::ConfigServiceError;

    ProviderNode(ConfigGraph &graph, Key key, nacos::Subscription<nacos::ConfigData> subscription);

    [[nodiscard]] const Key &key() const noexcept { return key_; }
    [[nodiscard]] const std::shared_ptr<const ProjectProvider> &current() const noexcept { return current_; }

    void start(std::shared_ptr<ProviderNode> self);
    void request_stop() noexcept;
    [[nodiscard]] bool on_service_initialized(const LoadBalancer &service);

private:
    struct Generation {
        std::shared_ptr<const ProviderConfigSnapshot> config;
        std::optional<ServiceDiscovery::Handle> service;

        [[nodiscard]] bool ready() const noexcept { return !service || service->load_balancer().initialized(); }
    };

    [[nodiscard]] static async::DetachedTask run(std::shared_ptr<ProviderNode> self) noexcept;
    void apply(const nacos::ConfigData &data);
    void activate_candidate();
    void rebuild_current();
    [[nodiscard]] static bool references(const Generation &generation, const LoadBalancer &service) noexcept;

    ConfigGraph *graph_ = nullptr;
    Key key_;
    nacos::Subscription<nacos::ConfigData> subscription_;
    std::optional<Generation> active_;
    std::optional<Generation> candidate_;
    std::shared_ptr<const ProjectProvider> current_;
    async::Watch<bool> stop_{false};
    std::optional<async::Watch<bool>::Publisher> stop_publisher_;
    bool stopping_ = false;
};

using ProviderNodePool = ConfigNodePool<ProviderNode>;

class ModelsNode final : public common::NonCopyable, public common::NonMovable {
public:
    ModelsNode(ConfigGraph &graph, nacos::Subscription<nacos::ConfigData> subscription);

    [[nodiscard]] const std::shared_ptr<const LlmProjectSnapshot> &current() const noexcept { return current_; }

    void start(std::shared_ptr<ModelsNode> self);
    void request_stop() noexcept;
    [[nodiscard]] bool on_provider_changed(std::string_view name);
    [[nodiscard]] bool on_providers_changed(const std::vector<std::string> &names);
    [[nodiscard]] bool on_group_changed(std::string_view name);

private:
    struct Generation {
        std::shared_ptr<const ModelsConfigSnapshot> config;
        std::map<std::string, ProviderNodePool::Ref, std::less<>> providers;
        std::map<std::string, GroupNodePool::Ref, std::less<>> groups;

        [[nodiscard]] bool ready() const noexcept;
    };

    [[nodiscard]] static async::DetachedTask run(std::shared_ptr<ModelsNode> self) noexcept;
    void apply(const nacos::ConfigData &data);
    void activate_candidate();
    void rebuild_current();
    [[nodiscard]] static bool references_provider(const Generation &generation, std::string_view name) noexcept;
    [[nodiscard]] static bool references_group(const Generation &generation, std::string_view name) noexcept;
    [[nodiscard]] bool candidate_ready_after_provider_change(const std::vector<std::string> &names) const noexcept;

    ConfigGraph *graph_ = nullptr;
    nacos::Subscription<nacos::ConfigData> subscription_;
    std::optional<Generation> active_;
    std::optional<Generation> candidate_;
    std::shared_ptr<const LlmProjectSnapshot> current_;
    std::uint64_t project_generation_ = 0;
    async::Watch<bool> stop_{false};
    std::optional<async::Watch<bool>::Publisher> stop_publisher_;
    bool stopping_ = false;
};

class Bt1Node final : public common::NonCopyable, public common::NonMovable {
public:
    Bt1Node(ConfigGraph &graph, nacos::Subscription<nacos::ConfigData> subscription);

    [[nodiscard]] const std::shared_ptr<const Bt1KeySnapshot> &current() const noexcept { return current_; }

    void start(std::shared_ptr<Bt1Node> self);
    void request_stop() noexcept;

private:
    [[nodiscard]] static async::DetachedTask run(std::shared_ptr<Bt1Node> self) noexcept;
    void apply(const nacos::ConfigData &data);

    ConfigGraph *graph_ = nullptr;
    nacos::Subscription<nacos::ConfigData> subscription_;
    std::shared_ptr<const Bt1KeySnapshot> current_;
    async::Watch<bool> stop_{false};
    std::optional<async::Watch<bool>::Publisher> stop_publisher_;
    bool stopping_ = false;
};

class ConfigGraph final : public common::NonCopyable, public common::NonMovable {
public:
    ConfigGraph(event::EventLoop &loop, nacos::ConfigService &config_service, nacos::NamingService &naming_service) :
        loop_(&loop), config_service_(&config_service),
        services_(loop, naming_service, ServiceDiscoveryOptions{.require_ip = true},
                  ServiceDiscoveryObserver{
                          .context = this,
                          .on_update = &ConfigGraph::service_updated,
                          .on_closed = &ConfigGraph::service_closed,
                  }),
        providers_(this, &ConfigGraph::create_provider_node), groups_(this, &ConfigGraph::create_group_node) {
        snapshot_publisher_ = snapshots_.acquire_publisher();
        FIBER_ASSERT(snapshot_publisher_.has_value());
    }

    ~ConfigGraph() {
        FIBER_ASSERT(state_ == LlmConfigManagerState::Created || state_ == LlmConfigManagerState::Stopped);
        FIBER_ASSERT(tasks_.empty());
        FIBER_ASSERT(providers_.empty());
        FIBER_ASSERT(groups_.empty());
        FIBER_ASSERT(services_.empty());
    }

    [[nodiscard]] std::expected<void, nacos::ConfigServiceError> start();
    [[nodiscard]] async::Task<void> shutdown() noexcept;

    [[nodiscard]] LlmConfigManagerState state() const noexcept { return state_; }
    [[nodiscard]] bool ready() const noexcept { return ready_; }
    [[nodiscard]] async::Watch<LlmConfigSnapshot>::Subscriber subscribe_snapshot() { return snapshots_.subscribe(); }
    [[nodiscard]] std::shared_ptr<const Bt1KeySnapshot> current_bt1_keys() const noexcept {
        return bt1_ ? bt1_->current() : nullptr;
    }
    [[nodiscard]] std::shared_ptr<const LlmProjectSnapshot> current_project() const noexcept {
        return models_ ? models_->current() : nullptr;
    }
    [[nodiscard]] const std::optional<LlmConfigFailure> &last_failure() const noexcept { return last_failure_; }
    [[nodiscard]] std::uint64_t successful_updates() const noexcept { return successful_updates_; }
    [[nodiscard]] std::uint64_t failed_updates() const noexcept { return failed_updates_; }
    [[nodiscard]] std::size_t provider_subscription_count() const noexcept { return providers_.size(); }
    [[nodiscard]] std::size_t user_group_subscription_count() const noexcept { return groups_.size(); }
    [[nodiscard]] std::size_t service_subscription_count() const noexcept { return services_.size(); }

    [[nodiscard]] event::EventLoop &loop() const noexcept { return *loop_; }
    [[nodiscard]] ProviderNodePool &providers() noexcept { return providers_; }
    [[nodiscard]] GroupNodePool &groups() noexcept { return groups_; }
    [[nodiscard]] ServiceDiscovery &services() noexcept { return services_; }

    void task_started() { tasks_.add(); }
    void task_done() noexcept { tasks_.done(); }
    void accepted_update() noexcept { ++successful_updates_; }

    void on_bt1_changed();
    void on_models_changed();
    void on_provider_changed(ProviderNode &provider);
    void on_group_changed(GroupNode &group);
    void on_service_initialized(LoadBalancer &service);

    void report_failure(std::string_view data_id, std::string_view md5, LlmConfigError error);
    void report_not_found(std::string_view data_id);

private:
    static void service_updated(void *context, LoadBalancer &service, std::string_view service_name,
                                std::string_view group, bool first_update, LoadBalancerUpdateResult result);
    static void service_closed(void *context, std::string_view service_name, std::string_view group);
    [[nodiscard]] static std::expected<std::shared_ptr<ProviderNode>, nacos::ConfigServiceError>
    create_provider_node(void *context, std::string key);
    [[nodiscard]] static std::expected<std::shared_ptr<GroupNode>, nacos::ConfigServiceError>
    create_group_node(void *context, std::string key);

    void publish_if_ready();

    event::EventLoop *loop_ = nullptr;
    nacos::ConfigService *config_service_ = nullptr;
    async::WaitGroup tasks_;
    ServiceDiscovery services_;
    ProviderNodePool providers_;
    GroupNodePool groups_;
    std::shared_ptr<Bt1Node> bt1_;
    std::shared_ptr<ModelsNode> models_;
    async::Watch<LlmConfigSnapshot> snapshots_;
    std::optional<async::Watch<LlmConfigSnapshot>::Publisher> snapshot_publisher_;
    std::optional<LlmConfigFailure> last_failure_;
    LlmConfigManagerState state_ = LlmConfigManagerState::Created;
    std::uint64_t snapshot_generation_ = 0;
    std::uint64_t successful_updates_ = 0;
    std::uint64_t failed_updates_ = 0;
    bool ready_ = false;
};

GroupNode::GroupNode(ConfigGraph &graph, Key key, nacos::Subscription<nacos::ConfigData> subscription) :
    graph_(&graph), key_(std::move(key)), subscription_(std::move(subscription)) {
    stop_publisher_ = stop_.acquire_publisher();
    FIBER_ASSERT(stop_publisher_.has_value());
}

void GroupNode::start(std::shared_ptr<GroupNode> self) {
    FIBER_ASSERT(graph_->loop().in_loop());
    graph_->task_started();
    async::spawn([self = std::move(self)]() mutable { return run(std::move(self)); });
}

void GroupNode::request_stop() noexcept {
    FIBER_ASSERT(graph_->loop().in_loop());
    if (!stopping_) {
        stopping_ = true;
        stop_publisher_->publish(true);
    }
}

async::DetachedTask GroupNode::run(std::shared_ptr<GroupNode> self) noexcept {
    auto stop = self->stop_.subscribe();
    auto stop_snapshot = stop.current();
    auto &subscriber = self->subscription_.subscriber();
    auto snapshot = subscriber.current();
    for (;;) {
        if (stop_snapshot.value && *stop_snapshot.value) {
            break;
        }
        if (snapshot.value) {
            if (snapshot.value->kind == nacos::ResultKind::Closed) {
                if (!self->stopping_) {
                    LOG(LOG_CONFIG, WARN) << "user-group config subscription closed group=" << log::quoted(self->key_);
                }
                break;
            }
            if (snapshot.value->data) {
                self->apply(*snapshot.value->data);
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
    self->subscription_.close();
    self->graph_->task_done();
}

void GroupNode::apply(const nacos::ConfigData &data) {
    FIBER_ASSERT(graph_->loop().in_loop());
    if (!graph_->groups().contains(*this)) {
        return;
    }
    const std::string data_id = std::string(kUserGroupDataIdPrefix) + key_;
    if (data.state == nacos::ConfigState::NotFound) {
        graph_->report_not_found(data_id);
        return;
    }
    auto parsed = parse_user_group_config(data.content, data.md5, key_);
    if (!parsed) {
        graph_->report_failure(data_id, data.md5, std::move(parsed.error()));
        return;
    }
    current_ = std::make_shared<const UserGroupSnapshot>(std::move(*parsed));
    LOG(LOG_CONFIG, DEBUG) << "user-group config accepted group=" << log::quoted(key_)
                           << " md5=" << log::quoted(current_->metadata.md5) << " users=" << current_->users.size();
    graph_->accepted_update();
    graph_->on_group_changed(*this);
}

ProviderNode::ProviderNode(ConfigGraph &graph, Key key, nacos::Subscription<nacos::ConfigData> subscription) :
    graph_(&graph), key_(std::move(key)), subscription_(std::move(subscription)) {
    stop_publisher_ = stop_.acquire_publisher();
    FIBER_ASSERT(stop_publisher_.has_value());
}

void ProviderNode::start(std::shared_ptr<ProviderNode> self) {
    FIBER_ASSERT(graph_->loop().in_loop());
    graph_->task_started();
    async::spawn([self = std::move(self)]() mutable { return run(std::move(self)); });
}

void ProviderNode::request_stop() noexcept {
    FIBER_ASSERT(graph_->loop().in_loop());
    if (stopping_) {
        return;
    }
    stopping_ = true;
    candidate_.reset();
    active_.reset();
    stop_publisher_->publish(true);
}

async::DetachedTask ProviderNode::run(std::shared_ptr<ProviderNode> self) noexcept {
    auto stop = self->stop_.subscribe();
    auto stop_snapshot = stop.current();
    auto &subscriber = self->subscription_.subscriber();
    auto snapshot = subscriber.current();
    for (;;) {
        if (stop_snapshot.value && *stop_snapshot.value) {
            break;
        }
        if (snapshot.value) {
            if (snapshot.value->kind == nacos::ResultKind::Closed) {
                if (!self->stopping_) {
                    LOG(LOG_CONFIG, WARN) << "provider config subscription closed provider=" << log::quoted(self->key_);
                }
                break;
            }
            if (snapshot.value->data) {
                self->apply(*snapshot.value->data);
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
    self->subscription_.close();
    self->graph_->task_done();
}

void ProviderNode::apply(const nacos::ConfigData &data) {
    FIBER_ASSERT(graph_->loop().in_loop());
    if (!graph_->providers().contains(*this)) {
        return;
    }
    const std::string data_id = std::string(kProviderDataIdPrefix) + key_;
    if (data.state == nacos::ConfigState::NotFound) {
        graph_->report_not_found(data_id);
        return;
    }
    auto parsed = parse_provider_config(data.content, data.md5, key_);
    if (!parsed) {
        graph_->report_failure(data_id, data.md5, std::move(parsed.error()));
        return;
    }

    Generation next;
    next.config = std::make_shared<const ProviderConfigSnapshot>(std::move(*parsed));
    constexpr std::string_view service_prefix = "service://";
    if (next.config->base_url.starts_with(service_prefix)) {
        auto service = graph_->services().acquire(next.config->base_url.substr(service_prefix.size()),
                                                  std::string(kDefaultNamingGroup));
        if (!service) {
            graph_->report_failure(data_id, data.md5,
                                   LlmConfigError{
                                           .code = LlmConfigErrorCode::InvalidField,
                                           .field = "data.baseurl",
                                           .message = service.error().message.empty()
                                                              ? "failed to subscribe to provider naming service"
                                                              : std::move(service.error().message),
                                   });
            return;
        }
        next.service.emplace(std::move(*service));
    }

    candidate_.emplace(std::move(next));
    LOG(LOG_CONFIG, DEBUG) << "provider config accepted provider=" << log::quoted(key_)
                           << " md5=" << log::quoted(candidate_->config->metadata.md5)
                           << " ready=" << candidate_->ready();
    graph_->accepted_update();
    if (candidate_->ready()) {
        activate_candidate();
        graph_->on_provider_changed(*this);
    }
}

bool ProviderNode::references(const Generation &generation, const LoadBalancer &service) noexcept {
    return generation.service && &generation.service->load_balancer() == &service;
}

bool ProviderNode::on_service_initialized(const LoadBalancer &service) {
    FIBER_ASSERT(graph_->loop().in_loop());
    if (candidate_ && references(*candidate_, service) && candidate_->ready()) {
        activate_candidate();
        return true;
    }
    return false;
}

void ProviderNode::activate_candidate() {
    FIBER_ASSERT(candidate_ && candidate_->ready());
    active_ = std::move(candidate_);
    candidate_.reset();
    rebuild_current();
}

void ProviderNode::rebuild_current() {
    FIBER_ASSERT(active_ && active_->ready());
    std::shared_ptr<LoadBalancer> service;
    if (active_->service) {
        service = active_->service->shared_load_balancer();
    }
    current_ = std::make_shared<const ProjectProvider>(ProjectProvider{
            .name = key_,
            .config = active_->config,
            .service = std::move(service),
    });
}

bool ModelsNode::Generation::ready() const noexcept {
    if (!config) {
        return false;
    }
    for (const auto &[name, provider]: providers) {
        (void) name;
        if (!provider.node().current()) {
            return false;
        }
    }
    for (const auto &[name, group]: groups) {
        (void) name;
        if (!group.node().current()) {
            return false;
        }
    }
    return true;
}

ModelsNode::ModelsNode(ConfigGraph &graph, nacos::Subscription<nacos::ConfigData> subscription) :
    graph_(&graph), subscription_(std::move(subscription)) {
    stop_publisher_ = stop_.acquire_publisher();
    FIBER_ASSERT(stop_publisher_.has_value());
}

void ModelsNode::start(std::shared_ptr<ModelsNode> self) {
    FIBER_ASSERT(graph_->loop().in_loop());
    graph_->task_started();
    async::spawn([self = std::move(self)]() mutable { return run(std::move(self)); });
}

void ModelsNode::request_stop() noexcept {
    FIBER_ASSERT(graph_->loop().in_loop());
    if (stopping_) {
        return;
    }
    stopping_ = true;
    candidate_.reset();
    active_.reset();
    stop_publisher_->publish(true);
}

async::DetachedTask ModelsNode::run(std::shared_ptr<ModelsNode> self) noexcept {
    auto stop = self->stop_.subscribe();
    auto stop_snapshot = stop.current();
    auto &subscriber = self->subscription_.subscriber();
    auto snapshot = subscriber.current();
    for (;;) {
        if (stop_snapshot.value && *stop_snapshot.value) {
            break;
        }
        if (snapshot.value) {
            if (snapshot.value->kind == nacos::ResultKind::Closed) {
                if (!self->stopping_) {
                    LOG(LOG_CONFIG, WARN) << "models config subscription closed";
                }
                break;
            }
            if (snapshot.value->data) {
                self->apply(*snapshot.value->data);
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
    self->subscription_.close();
    self->graph_->task_done();
}

void ModelsNode::apply(const nacos::ConfigData &data) {
    FIBER_ASSERT(graph_->loop().in_loop());
    if (data.state == nacos::ConfigState::NotFound) {
        graph_->report_not_found(kModelsDataId);
        return;
    }
    auto parsed = parse_models_config(data.content, data.md5);
    if (!parsed) {
        graph_->report_failure(kModelsDataId, data.md5, std::move(parsed.error()));
        return;
    }

    Generation next;
    next.config = std::make_shared<const ModelsConfigSnapshot>(std::move(*parsed));
    for (const ModelDefinition &model: next.config->models) {
        for (const std::string &name: model.providers) {
            if (next.providers.contains(name)) {
                continue;
            }
            auto provider = graph_->providers().acquire(name);
            if (!provider) {
                graph_->report_failure(kModelsDataId, data.md5,
                                       LlmConfigError{
                                               .code = LlmConfigErrorCode::InvalidField,
                                               .field = "dependencies",
                                               .message = provider.error().message.empty()
                                                                  ? "failed to subscribe to model provider"
                                                                  : std::move(provider.error().message),
                                       });
                return;
            }
            next.providers.emplace(name, std::move(*provider));
        }
        if (model.fallback_provider && !next.providers.contains(*model.fallback_provider)) {
            auto provider = graph_->providers().acquire(*model.fallback_provider);
            if (!provider) {
                graph_->report_failure(kModelsDataId, data.md5,
                                       LlmConfigError{
                                               .code = LlmConfigErrorCode::InvalidField,
                                               .field = "dependencies",
                                               .message = provider.error().message.empty()
                                                                  ? "failed to subscribe to fallback provider"
                                                                  : std::move(provider.error().message),
                                       });
                return;
            }
            next.providers.emplace(*model.fallback_provider, std::move(*provider));
        }
        for (const std::string &name: model.allow_user_groups) {
            if (next.groups.contains(name)) {
                continue;
            }
            auto group = graph_->groups().acquire(name);
            if (!group) {
                graph_->report_failure(kModelsDataId, data.md5,
                                       LlmConfigError{
                                               .code = LlmConfigErrorCode::InvalidField,
                                               .field = "dependencies",
                                               .message = group.error().message.empty()
                                                                  ? "failed to subscribe to model user group"
                                                                  : std::move(group.error().message),
                                       });
                return;
            }
            next.groups.emplace(name, std::move(*group));
        }
    }

    candidate_.emplace(std::move(next));
    LOG(LOG_CONFIG, DEBUG) << "models config accepted md5=" << log::quoted(candidate_->config->metadata.md5)
                           << " models=" << candidate_->config->models.size()
                           << " providers=" << candidate_->providers.size() << " groups=" << candidate_->groups.size()
                           << " ready=" << candidate_->ready();
    graph_->accepted_update();
    if (candidate_->ready()) {
        activate_candidate();
        graph_->on_models_changed();
    }
}

bool ModelsNode::references_provider(const Generation &generation, std::string_view name) noexcept {
    return generation.providers.contains(name);
}

bool ModelsNode::references_group(const Generation &generation, std::string_view name) noexcept {
    return generation.groups.contains(name);
}

bool ModelsNode::on_provider_changed(std::string_view name) {
    const std::vector<std::string> names{std::string(name)};
    return on_providers_changed(names);
}

bool ModelsNode::candidate_ready_after_provider_change(const std::vector<std::string> &names) const noexcept {
    if (!candidate_) {
        return false;
    }
    for (const std::string &name: names) {
        if (references_provider(*candidate_, name)) {
            return candidate_->ready();
        }
    }
    return false;
}

bool ModelsNode::on_providers_changed(const std::vector<std::string> &names) {
    FIBER_ASSERT(graph_->loop().in_loop());
    if (candidate_ready_after_provider_change(names)) {
        activate_candidate();
        return true;
    }
    if (!active_) {
        return false;
    }
    for (const std::string &name: names) {
        if (references_provider(*active_, name)) {
            rebuild_current();
            return true;
        }
    }
    return false;
}

bool ModelsNode::on_group_changed(std::string_view name) {
    FIBER_ASSERT(graph_->loop().in_loop());
    if (candidate_ && references_group(*candidate_, name) && candidate_->ready()) {
        activate_candidate();
        return true;
    }
    if (active_ && references_group(*active_, name)) {
        rebuild_current();
        return true;
    }
    return false;
}

void ModelsNode::activate_candidate() {
    FIBER_ASSERT(candidate_ && candidate_->ready());
    active_ = std::move(candidate_);
    candidate_.reset();
    rebuild_current();
}

void ModelsNode::rebuild_current() {
    FIBER_ASSERT(active_ && active_->ready());
    FIBER_ASSERT(project_generation_ != std::numeric_limits<std::uint64_t>::max());

    std::vector<std::shared_ptr<const ProjectProvider>> providers;
    providers.reserve(active_->providers.size());
    for (const auto &[name, provider]: active_->providers) {
        (void) name;
        FIBER_ASSERT(provider.node().current() != nullptr);
        providers.push_back(provider.node().current());
    }

    const std::int64_t rate_limit_revision =
            active_->config->metadata.md5.empty()
                    ? static_cast<std::int64_t>(active_->config->metadata.version)
                    : std::bit_cast<std::int64_t>(rate_limit_hash64(active_->config->metadata.md5));
    std::vector<CompiledModelRoute> routes;
    routes.reserve(active_->config->models.size());
    for (const ModelDefinition &model: active_->config->models) {
        CompiledModelRoute route;
        route.model_name = model.model_name;
        route.providers.reserve(model.providers.size());
        for (const std::string &name: model.providers) {
            const auto provider = active_->providers.find(name);
            FIBER_ASSERT(provider != active_->providers.end());
            route.providers.push_back(provider->second.node().current());
        }
        if (model.fallback_provider) {
            const auto provider = active_->providers.find(*model.fallback_provider);
            FIBER_ASSERT(provider != active_->providers.end());
            route.fallback_provider = provider->second.node().current();
        }
        route.allow_user_groups.reserve(model.allow_user_groups.size());
        for (const std::string &name: model.allow_user_groups) {
            const auto group = active_->groups.find(name);
            FIBER_ASSERT(group != active_->groups.end());
            route.allow_user_groups.push_back(group->second.node().current());
        }
        route.load_balance = model.load_balance;
        if (model.rate_limit) {
            route.rate_limit = CompiledModelRateLimitRule{
                    .revision = rate_limit_revision,
                    .window_duration_millis = model.rate_limit->window_duration_millis,
                    .max_tokens_per_window = model.rate_limit->max_tokens_per_window,
            };
        }
        routes.push_back(std::move(route));
    }

    current_ = std::make_shared<const LlmProjectSnapshot>(active_->config->metadata, ++project_generation_,
                                                          std::move(providers), std::move(routes));
}

Bt1Node::Bt1Node(ConfigGraph &graph, nacos::Subscription<nacos::ConfigData> subscription) :
    graph_(&graph), subscription_(std::move(subscription)) {
    stop_publisher_ = stop_.acquire_publisher();
    FIBER_ASSERT(stop_publisher_.has_value());
}

void Bt1Node::start(std::shared_ptr<Bt1Node> self) {
    FIBER_ASSERT(graph_->loop().in_loop());
    graph_->task_started();
    async::spawn([self = std::move(self)]() mutable { return run(std::move(self)); });
}

void Bt1Node::request_stop() noexcept {
    FIBER_ASSERT(graph_->loop().in_loop());
    if (!stopping_) {
        stopping_ = true;
        stop_publisher_->publish(true);
    }
}

async::DetachedTask Bt1Node::run(std::shared_ptr<Bt1Node> self) noexcept {
    auto stop = self->stop_.subscribe();
    auto stop_snapshot = stop.current();
    auto &subscriber = self->subscription_.subscriber();
    auto snapshot = subscriber.current();
    for (;;) {
        if (stop_snapshot.value && *stop_snapshot.value) {
            break;
        }
        if (snapshot.value) {
            if (snapshot.value->kind == nacos::ResultKind::Closed) {
                if (!self->stopping_) {
                    LOG(LOG_CONFIG, WARN) << "BT1 key config subscription closed";
                }
                break;
            }
            if (snapshot.value->data) {
                self->apply(*snapshot.value->data);
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
    self->subscription_.close();
    self->graph_->task_done();
}

void Bt1Node::apply(const nacos::ConfigData &data) {
    FIBER_ASSERT(graph_->loop().in_loop());
    if (data.state == nacos::ConfigState::NotFound) {
        graph_->report_not_found(kBt1KeysDataId);
        return;
    }
    auto parsed = parse_bt1_key_config(data.content, data.md5);
    if (!parsed) {
        graph_->report_failure(kBt1KeysDataId, data.md5, std::move(parsed.error()));
        return;
    }
    current_ = std::make_shared<const Bt1KeySnapshot>(std::move(*parsed));
    LOG(LOG_CONFIG, DEBUG) << "BT1 key config accepted md5=" << log::quoted(current_->metadata.md5)
                           << " keys=" << current_->keys.size();
    graph_->accepted_update();
    graph_->on_bt1_changed();
}

std::expected<void, nacos::ConfigServiceError> ConfigGraph::start() {
    FIBER_ASSERT(loop_->in_loop());
    if (state_ != LlmConfigManagerState::Created) {
        return std::unexpected(nacos::ConfigServiceError{
                .code = nacos::ConfigServiceErrorCode::InvalidArgument,
                .io_error = common::IoErr::Already,
                .message = "LLM config manager is already started",
        });
    }

    auto bt1_subscription = config_service_->subscribe(kBt1KeysDataId, kLlmConfigGroup);
    if (!bt1_subscription) {
        return std::unexpected(std::move(bt1_subscription.error()));
    }
    auto models_subscription = config_service_->subscribe(kModelsDataId, kLlmConfigGroup);
    if (!models_subscription) {
        return std::unexpected(std::move(models_subscription.error()));
    }

    bt1_ = std::make_shared<Bt1Node>(*this, std::move(*bt1_subscription));
    models_ = std::make_shared<ModelsNode>(*this, std::move(*models_subscription));
    state_ = LlmConfigManagerState::Running;
    bt1_->start(bt1_);
    models_->start(models_);
    LOG(LOG_CONFIG, INFO) << "LLM config graph subscriptions started group=" << log::quoted(kLlmConfigGroup);
    return {};
}

async::Task<void> ConfigGraph::shutdown() noexcept {
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
        models_->request_stop();
        bt1_->request_stop();
    }
    co_await tasks_.join();
    models_.reset();
    bt1_.reset();
    FIBER_ASSERT(providers_.empty());
    FIBER_ASSERT(groups_.empty());
    FIBER_ASSERT(services_.empty());
    co_await services_.shutdown();
    state_ = LlmConfigManagerState::Stopped;
    LOG(LOG_CONFIG, INFO) << "LLM config graph stopped successful_updates=" << successful_updates_
                          << " failed_updates=" << failed_updates_;
}

std::expected<std::shared_ptr<ProviderNode>, nacos::ConfigServiceError>
ConfigGraph::create_provider_node(void *context, std::string key) {
    auto &graph = *static_cast<ConfigGraph *>(context);
    FIBER_ASSERT(graph.loop_->in_loop());
    const std::string data_id = std::string(kProviderDataIdPrefix) + key;
    auto subscription = graph.config_service_->subscribe(data_id, kLlmConfigGroup);
    if (!subscription) {
        return std::unexpected(std::move(subscription.error()));
    }
    return std::make_shared<ProviderNode>(graph, std::move(key), std::move(*subscription));
}

std::expected<std::shared_ptr<GroupNode>, nacos::ConfigServiceError> ConfigGraph::create_group_node(void *context,
                                                                                                    std::string key) {
    auto &graph = *static_cast<ConfigGraph *>(context);
    FIBER_ASSERT(graph.loop_->in_loop());
    const std::string data_id = std::string(kUserGroupDataIdPrefix) + key;
    auto subscription = graph.config_service_->subscribe(data_id, kLlmConfigGroup);
    if (!subscription) {
        return std::unexpected(std::move(subscription.error()));
    }
    return std::make_shared<GroupNode>(graph, std::move(key), std::move(*subscription));
}

void ConfigGraph::on_bt1_changed() {
    FIBER_ASSERT(loop_->in_loop());
    publish_if_ready();
}

void ConfigGraph::on_models_changed() {
    FIBER_ASSERT(loop_->in_loop());
    publish_if_ready();
}

void ConfigGraph::on_provider_changed(ProviderNode &provider) {
    FIBER_ASSERT(loop_->in_loop());
    if (!providers_.contains(provider) || !models_) {
        return;
    }
    if (models_->on_provider_changed(provider.key())) {
        publish_if_ready();
    }
}

void ConfigGraph::on_group_changed(GroupNode &group) {
    FIBER_ASSERT(loop_->in_loop());
    if (!groups_.contains(group) || !models_) {
        return;
    }
    if (models_->on_group_changed(group.key())) {
        publish_if_ready();
    }
}

void ConfigGraph::service_updated(void *context, LoadBalancer &service, std::string_view service_name,
                                  std::string_view group, bool first_update, LoadBalancerUpdateResult result) {
    auto &graph = *static_cast<ConfigGraph *>(context);
    LOG(LOG_DISCOVERY, DEBUG) << "NamingService instances updated service=" << log::quoted(service_name)
                              << " group=" << log::quoted(group) << " generation=" << service.generation()
                              << " instances=" << service.configured_instance_count()
                              << " changed=" << (result == LoadBalancerUpdateResult::Applied);
    graph.accepted_update();
    if (first_update) {
        graph.on_service_initialized(service);
    }
}

void ConfigGraph::service_closed(void *, std::string_view service_name, std::string_view group) {
    LOG(LOG_DISCOVERY, WARN) << "NamingService subscription closed service=" << log::quoted(service_name)
                             << " group=" << log::quoted(group);
}

void ConfigGraph::on_service_initialized(LoadBalancer &service) {
    FIBER_ASSERT(loop_->in_loop());
    if (!models_) {
        return;
    }
    std::vector<std::string> changed;
    providers_.for_each([&](ProviderNode &provider) {
        if (provider.on_service_initialized(service)) {
            changed.push_back(provider.key());
        }
    });
    if (!changed.empty() && models_->on_providers_changed(changed)) {
        publish_if_ready();
    }
}

void ConfigGraph::publish_if_ready() {
    FIBER_ASSERT(loop_->in_loop());
    if (state_ != LlmConfigManagerState::Running || !bt1_ || !models_ || !bt1_->current() || !models_->current()) {
        return;
    }
    FIBER_ASSERT(snapshot_generation_ != std::numeric_limits<std::uint64_t>::max());
    const bool initial = !ready_;
    const auto project = models_->current();
    const auto bt1_keys = bt1_->current();
    const std::uint64_t generation = ++snapshot_generation_;
    snapshot_publisher_->publish(LlmConfigSnapshot{
            .generation = generation,
            .bt1_keys = bt1_keys,
            .project = project,
    });
    ready_ = true;
    LOG(LOG_CONFIG, INFO) << (initial ? "initial LLM config snapshot published" : "LLM config snapshot published")
                          << " generation=" << generation << " project_generation=" << project->generation()
                          << " models=" << project->models().size() << " providers=" << project->providers().size()
                          << " group_subscriptions=" << groups_.size() << " service_subscriptions=" << services_.size()
                          << " bt1_keys=" << bt1_keys->keys.size();
}

void ConfigGraph::report_failure(std::string_view data_id, std::string_view md5, LlmConfigError error) {
    ++failed_updates_;
    log_config_rejection(ready_, data_id, md5, error);
    last_failure_ = LlmConfigFailure{
            .data_id = std::string(data_id),
            .md5 = std::string(md5),
            .error = std::move(error),
    };
}

void ConfigGraph::report_not_found(std::string_view data_id) {
    report_failure(data_id, {},
                   LlmConfigError{
                           .code = LlmConfigErrorCode::InvalidEnvelope,
                           .field = "data",
                           .message = "configuration is not found; retaining the last valid snapshot",
                   });
}

} // namespace

class LlmConfigManager::Impl final : public common::NonCopyable, public common::NonMovable {
public:
    Impl(event::EventLoop &loop, nacos::ConfigService &config_service, nacos::NamingService &naming_service) :
        graph_(loop, config_service, naming_service) {}

    ConfigGraph graph_;
};

LlmConfigManager::LlmConfigManager(event::EventLoop &loop, nacos::ConfigService &config_service,
                                   nacos::NamingService &naming_service) :
    impl_(std::make_unique<Impl>(loop, config_service, naming_service)) {}

LlmConfigManager::~LlmConfigManager() = default;

std::expected<void, nacos::ConfigServiceError> LlmConfigManager::start() { return impl_->graph_.start(); }

async::Task<void> LlmConfigManager::shutdown() noexcept { co_await impl_->graph_.shutdown(); }

LlmConfigManagerState LlmConfigManager::state() const noexcept { return impl_->graph_.state(); }

bool LlmConfigManager::ready() const noexcept { return impl_->graph_.ready(); }

LlmConfigManager::SnapshotSubscriber LlmConfigManager::subscribe_snapshot() {
    return impl_->graph_.subscribe_snapshot();
}

std::shared_ptr<const Bt1KeySnapshot> LlmConfigManager::current_bt1_keys() const noexcept {
    return impl_->graph_.current_bt1_keys();
}

std::shared_ptr<const LlmProjectSnapshot> LlmConfigManager::current_project() const noexcept {
    return impl_->graph_.current_project();
}

const std::optional<LlmConfigFailure> &LlmConfigManager::last_failure() const noexcept {
    return impl_->graph_.last_failure();
}

std::uint64_t LlmConfigManager::successful_updates() const noexcept { return impl_->graph_.successful_updates(); }

std::uint64_t LlmConfigManager::failed_updates() const noexcept { return impl_->graph_.failed_updates(); }

std::size_t LlmConfigManager::provider_subscription_count() const noexcept {
    return impl_->graph_.provider_subscription_count();
}

std::size_t LlmConfigManager::user_group_subscription_count() const noexcept {
    return impl_->graph_.user_group_subscription_count();
}

std::size_t LlmConfigManager::service_subscription_count() const noexcept {
    return impl_->graph_.service_subscription_count();
}

} // namespace fiber::ai_server
