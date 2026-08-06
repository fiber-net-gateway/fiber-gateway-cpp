#ifndef FIBER_AI_SERVER_MCP_CONFIG_MANAGER_H
#define FIBER_AI_SERVER_MCP_CONFIG_MANAGER_H

#include "McpConfigSnapshot.h"
#include "McpToolLoader.h"

#include <atomic>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include <async/Spawn.h>
#include <async/Task.h>
#include <async/WaitGroup.h>
#include <common/NonCopyable.h>
#include <common/NonMovable.h>
#include <event/EventLoop.h>
#include <fiber/nacos/ConfigService.h>
#include <fiber/nacos/NamingService.h>

namespace fiber::http_script {
class HttpScriptServices;
}

namespace fiber::ai_server {

class McpSessionManager;

enum class McpConfigManagerState : std::uint8_t {
    Created,
    Running,
    Stopping,
    Stopped,
};

struct McpConfigFailure {
    std::string data_id;
    std::string message;
};

class McpConfigManager final : public common::NonCopyable, public common::NonMovable {
public:
    McpConfigManager(event::EventLoop &loop, nacos::ConfigService &config_service, nacos::NamingService &naming_service,
                     std::filesystem::path cache_directory = "cache/ai",
                     http_script::HttpScriptServices *script_services = nullptr);
    ~McpConfigManager();

    [[nodiscard]] std::expected<void, nacos::ConfigServiceError> start();
    [[nodiscard]] async::Task<void> shutdown() noexcept;

    [[nodiscard]] McpConfigStore &store() noexcept { return store_; }
    [[nodiscard]] const McpConfigStore &store() const noexcept { return store_; }
    void set_session_manager(McpSessionManager *sessions) noexcept;
    [[nodiscard]] McpConfigManagerState state() const noexcept { return state_; }
    [[nodiscard]] std::size_t project_subscription_count() const noexcept { return projects_.size(); }
    [[nodiscard]] std::uint64_t successful_updates() const noexcept { return successful_updates_; }
    [[nodiscard]] std::uint64_t failed_updates() const noexcept { return failed_updates_; }
    [[nodiscard]] const std::optional<McpConfigFailure> &last_failure() const noexcept { return last_failure_; }

private:
    struct ProjectNode;

    static void projects_notify(void *context, const nacos::SubscriptionResult<nacos::ConfigData> &result) noexcept;
    static void tools_notify(void *context, const nacos::SubscriptionResult<nacos::ConfigData> &result) noexcept;

    void apply_projects(const nacos::ConfigData &data);
    void apply_tools(ProjectNode &node, const nacos::ConfigData &data);
    [[nodiscard]] std::expected<std::shared_ptr<ProjectNode>, nacos::ConfigServiceError>
    create_project(std::string name);
    [[nodiscard]] async::DetachedTask rebuild_project(std::shared_ptr<ProjectNode> node, std::uint64_t revision,
                                                      std::int32_t config_version,
                                                      std::vector<std::string> tool_ids) noexcept;
    void publish_snapshot(std::string_view changed_project = {});
    void report_failure(std::string data_id, std::string message);

    event::EventLoop *loop_ = nullptr;
    nacos::ConfigService *config_service_ = nullptr;
    McpToolLoader tool_loader_;
    McpConfigStore store_;
    nacos::Subscription<nacos::ConfigData> projects_subscription_;
    std::map<std::string, std::shared_ptr<ProjectNode>, std::less<>> projects_;
    std::map<std::string, std::weak_ptr<const McpTool>, std::less<>> tool_cache_;
    async::WaitGroup rebuild_tasks_;
    std::atomic<McpSessionManager *> sessions_{nullptr};
    std::optional<McpConfigFailure> last_failure_;
    McpConfigManagerState state_ = McpConfigManagerState::Created;
    std::int32_t projects_version_ = -1;
    std::uint64_t snapshot_generation_ = 0;
    std::uint64_t successful_updates_ = 0;
    std::uint64_t failed_updates_ = 0;
};

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_MCP_CONFIG_MANAGER_H
