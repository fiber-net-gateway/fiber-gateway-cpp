#include "McpConfigManager.h"

#include "../observability/AiServerLogCategories.h"
#include "McpJsonCodec.h"
#include "McpProtocol.h"
#include "McpSessionManager.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include <async/Sleep.h>
#include <async/Spawn.h>
#include <common/Assert.h>
#include <log/Log.h>

namespace fiber::ai_server {
namespace {

DEFINE_LOGGER(LOG_MCP_CONFIG, kAiServerConfigLogger);

std::string tools_data_id(std::string_view project) {
    std::string output(kMcpToolsDataIdPrefix);
    output.append(project);
    return output;
}

} // namespace

struct McpConfigManager::ProjectNode final : public common::NonCopyable,
                                             public common::NonMovable,
                                             public std::enable_shared_from_this<ProjectNode> {
    McpConfigManager *manager = nullptr;
    std::string name;
    std::string data_id;
    nacos::Subscription<nacos::ConfigData> subscription;
    std::shared_ptr<const McpProjectRuntime> runtime;
    std::vector<std::string> tool_ids;
    std::int32_t config_version = -1;
    std::uint64_t revision = 0;
    bool stopping = false;
};

McpConfigManager::McpConfigManager(event::EventLoop &loop, nacos::ConfigService &config_service,
                                   nacos::NamingService &naming_service, std::filesystem::path cache_directory,
                                   http_script::HttpScriptServices *script_services) :
    loop_(&loop), config_service_(&config_service),
    tool_loader_(loop, naming_service, std::move(cache_directory), script_services) {}

McpConfigManager::~McpConfigManager() {
    FIBER_ASSERT(state_ == McpConfigManagerState::Created || state_ == McpConfigManagerState::Stopped);
    FIBER_ASSERT(projects_.empty());
    FIBER_ASSERT(rebuild_tasks_.empty());
}

std::expected<void, nacos::ConfigServiceError> McpConfigManager::start() {
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(state_ == McpConfigManagerState::Created);
    auto loader_started = tool_loader_.start();
    if (!loader_started) {
        return std::unexpected(nacos::ConfigServiceError{
                .code = nacos::ConfigServiceErrorCode::Server,
                .io_error = loader_started.error().io_error,
                .message = std::move(loader_started.error().message),
        });
    }
    state_ = McpConfigManagerState::Running;
    auto subscription = config_service_->subscribe(kMcpProjectsDataId, kMcpConfigGroup, &projects_notify, this);
    if (!subscription) {
        state_ = McpConfigManagerState::Stopping;
        tool_loader_.stop();
        state_ = McpConfigManagerState::Stopped;
        return std::unexpected(std::move(subscription.error()));
    }
    projects_subscription_ = std::move(*subscription);
    return {};
}

void McpConfigManager::set_session_manager(McpSessionManager *sessions) noexcept {
    sessions_.store(sessions, std::memory_order_release);
}

void McpConfigManager::projects_notify(void *context,
                                       const nacos::SubscriptionResult<nacos::ConfigData> &result) noexcept {
    auto &self = *static_cast<McpConfigManager *>(context);
    if (result.kind == nacos::ResultKind::Closed || !result.data || self.state_ != McpConfigManagerState::Running) {
        return;
    }
    self.apply_projects(*result.data);
}

void McpConfigManager::tools_notify(void *context,
                                    const nacos::SubscriptionResult<nacos::ConfigData> &result) noexcept {
    auto &node = *static_cast<ProjectNode *>(context);
    if (result.kind == nacos::ResultKind::Closed || !result.data || node.stopping ||
        node.manager->state_ != McpConfigManagerState::Running) {
        return;
    }
    node.manager->apply_tools(node, *result.data);
}

void McpConfigManager::apply_projects(const nacos::ConfigData &data) {
    FIBER_ASSERT(loop_->in_loop());
    if (data.state == nacos::ConfigState::NotFound) {
        for (auto &[name, node]: projects_) {
            (void) name;
            node->stopping = true;
            ++node->revision;
            node->subscription.close();
        }
        projects_.clear();
        projects_version_ = -1;
        ++successful_updates_;
        publish_snapshot();
        return;
    }
    auto parsed = parse_mcp_name_set_config(data.content, true);
    if (!parsed) {
        report_failure(std::string(kMcpProjectsDataId), std::move(parsed.error().message));
        return;
    }
    if (parsed->version == projects_version_) {
        return;
    }

    std::map<std::string, std::shared_ptr<ProjectNode>, std::less<>> next;
    for (std::string &name: parsed->names) {
        auto current = projects_.find(name);
        if (current != projects_.end()) {
            next.emplace(name, current->second);
            continue;
        }
        auto created = create_project(name);
        if (!created) {
            report_failure(std::string(kMcpProjectsDataId), std::move(created.error().message));
            for (auto &[next_name, node]: next) {
                if (!projects_.contains(next_name)) {
                    node->stopping = true;
                    node->subscription.close();
                }
            }
            return;
        }
        next.emplace(name, std::move(*created));
    }
    for (auto &[name, node]: projects_) {
        if (!next.contains(name)) {
            node->stopping = true;
            ++node->revision;
            node->subscription.close();
        }
    }
    projects_ = std::move(next);
    projects_version_ = parsed->version;
    ++successful_updates_;
    publish_snapshot();
}

std::expected<std::shared_ptr<McpConfigManager::ProjectNode>, nacos::ConfigServiceError>
McpConfigManager::create_project(std::string name) {
    auto node = std::make_shared<ProjectNode>();
    node->manager = this;
    node->name = std::move(name);
    node->data_id = tools_data_id(node->name);
    auto subscription = config_service_->subscribe(node->data_id, kMcpConfigGroup, &tools_notify, node.get());
    if (!subscription) {
        return std::unexpected(std::move(subscription.error()));
    }
    node->subscription = std::move(*subscription);
    return node;
}

void McpConfigManager::apply_tools(ProjectNode &node, const nacos::ConfigData &data) {
    FIBER_ASSERT(loop_->in_loop());
    if (data.state == nacos::ConfigState::NotFound) {
        report_failure(node.data_id, "MCP project tool configuration is not found");
        return;
    }
    auto parsed = parse_mcp_name_set_config(data.content, false);
    if (!parsed) {
        report_failure(node.data_id, std::move(parsed.error().message));
        return;
    }
    if (parsed->names.empty()) {
        report_failure(node.data_id, "MCP project must contain at least one tool");
        return;
    }
    if (parsed->version == node.config_version && parsed->names == node.tool_ids) {
        return;
    }
    const std::uint64_t revision = ++node.revision;
    rebuild_tasks_.add();
    async::spawn([this, shared = node.shared_from_this(), revision, config_version = parsed->version,
                  tool_ids = std::move(parsed->names)]() mutable {
        return rebuild_project(std::move(shared), revision, config_version, std::move(tool_ids));
    });
}

async::DetachedTask McpConfigManager::rebuild_project(std::shared_ptr<ProjectNode> node, std::uint64_t revision,
                                                      std::int32_t config_version,
                                                      std::vector<std::string> tool_ids) noexcept {
    FIBER_ASSERT(loop_->in_loop());
    std::vector<std::shared_ptr<const McpTool>> tools;
    tools.reserve(tool_ids.size());
    std::string failure;
    for (const std::string &tool_id: tool_ids) {
        std::shared_ptr<const McpTool> tool;
        const auto cached = tool_cache_.find(tool_id);
        if (cached != tool_cache_.end()) {
            tool = cached->second.lock();
        }
        while (!tool && !node->stopping && node->revision == revision && state_ == McpConfigManagerState::Running) {
            auto loaded = co_await tool_loader_.load(tool_id);
            if (loaded) {
                tool = std::move(*loaded);
                tool_cache_[tool_id] = tool;
                break;
            }
            if (loaded.error().code != McpToolLoadErrorCode::NoAdminInstance) {
                failure = std::move(loaded.error().message);
                break;
            }
            co_await async::sleep(std::chrono::seconds(1));
        }
        if (!tool) {
            if (failure.empty() && !node->stopping && node->revision == revision) {
                failure = "MCP tool loading stopped";
            }
            break;
        }
        tools.push_back(std::move(tool));
    }
    if (!failure.empty() && !node->stopping && node->revision == revision && state_ == McpConfigManagerState::Running) {
        report_failure(node->data_id, std::move(failure));
    } else if (tools.size() == tool_ids.size() && !node->stopping && node->revision == revision &&
               state_ == McpConfigManagerState::Running) {
        std::sort(tools.begin(), tools.end(),
                  [](const auto &left, const auto &right) { return left->descriptor.name < right->descriptor.name; });
        bool duplicate = false;
        for (std::size_t i = 1; i < tools.size(); ++i) {
            if (tools[i - 1]->descriptor.name == tools[i]->descriptor.name) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            report_failure(node->data_id, "MCP project contains duplicate tool names");
        } else {
            auto runtime = std::make_shared<McpProjectRuntime>();
            runtime->name = node->name;
            runtime->tools = std::move(tools);
            node->runtime = std::move(runtime);
            node->tool_ids = std::move(tool_ids);
            node->config_version = config_version;
            ++successful_updates_;
            publish_snapshot(node->name);
        }
    }
    rebuild_tasks_.done();
}

void McpConfigManager::publish_snapshot(std::string_view changed_project) {
    auto snapshot = std::make_shared<McpConfigSnapshot>();
    snapshot->generation = ++snapshot_generation_;
    snapshot->projects.reserve(projects_.size());
    for (const auto &[name, node]: projects_) {
        (void) name;
        if (node->runtime) {
            snapshot->projects.push_back(node->runtime);
        }
    }
    store_.update(std::move(snapshot));
    if (!changed_project.empty()) {
        if (McpSessionManager *manager = sessions_.load(std::memory_order_acquire)) {
            const auto changed = projects_.find(changed_project);
            if (changed != projects_.end() && changed->second->runtime) {
                manager->update_project(changed_project, changed->second->runtime,
                                        McpProtocol::tools_list_changed_notification());
            }
        }
    }
}

void McpConfigManager::report_failure(std::string data_id, std::string message) {
    ++failed_updates_;
    last_failure_ = McpConfigFailure{
            .data_id = std::move(data_id),
            .message = std::move(message),
    };
    LOG(LOG_MCP_CONFIG, WARN) << "MCP config update rejected data_id=" << log::quoted(last_failure_->data_id)
                              << " reason=" << log::quoted(last_failure_->message);
}

async::Task<void> McpConfigManager::shutdown() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (state_ == McpConfigManagerState::Stopped) {
        co_return;
    }
    if (state_ == McpConfigManagerState::Created) {
        state_ = McpConfigManagerState::Stopped;
        co_return;
    }
    state_ = McpConfigManagerState::Stopping;
    projects_subscription_.close();
    for (auto &[name, node]: projects_) {
        (void) name;
        node->stopping = true;
        ++node->revision;
        node->subscription.close();
    }
    co_await rebuild_tasks_.join();
    projects_.clear();
    tool_cache_.clear();
    store_.update(std::make_shared<const McpConfigSnapshot>());
    co_await tool_loader_.shutdown();
    state_ = McpConfigManagerState::Stopped;
}

} // namespace fiber::ai_server
