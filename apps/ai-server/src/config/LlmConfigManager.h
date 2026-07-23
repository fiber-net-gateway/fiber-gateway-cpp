#ifndef FIBER_AI_SERVER_LLM_CONFIG_MANAGER_H
#define FIBER_AI_SERVER_LLM_CONFIG_MANAGER_H

#include "LlmConfigCodec.h"
#include "LlmConfigSnapshot.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include <async/Spawn.h>
#include <async/Task.h>
#include <async/WaitGroup.h>
#include <async/Watch.h>
#include <common/NonCopyable.h>
#include <common/NonMovable.h>
#include <event/EventLoop.h>
#include <fiber/nacos/ConfigService.h>
#include <fiber/nacos/Subscription.h>

namespace fiber::ai_server {

enum class LlmConfigManagerState : std::uint8_t {
    Created,
    Running,
    Stopping,
    Stopped,
};

struct LlmConfigFailure {
    std::string data_id;
    std::string md5;
    LlmConfigError error;
};

class LlmConfigManager final : public common::NonCopyable, public common::NonMovable {
public:
    LlmConfigManager(event::EventLoop &loop, nacos::ConfigService &config_service);
    ~LlmConfigManager();

    [[nodiscard]] std::expected<void, nacos::ConfigServiceError> start();
    [[nodiscard]] async::Task<void> shutdown() noexcept;

    [[nodiscard]] LlmConfigManagerState state() const noexcept { return state_; }
    [[nodiscard]] bool ready() const noexcept { return bt1_keys_ != nullptr && project_ != nullptr; }
    [[nodiscard]] std::shared_ptr<const Bt1KeySnapshot> current_bt1_keys() const noexcept { return bt1_keys_; }
    [[nodiscard]] std::shared_ptr<const LlmProjectSnapshot> current_project() const noexcept { return project_; }
    [[nodiscard]] const std::optional<LlmConfigFailure> &last_failure() const noexcept { return last_failure_; }
    [[nodiscard]] std::uint64_t successful_updates() const noexcept { return successful_updates_; }
    [[nodiscard]] std::uint64_t failed_updates() const noexcept { return failed_updates_; }
    [[nodiscard]] std::size_t provider_subscription_count() const noexcept { return providers_.size(); }
    [[nodiscard]] std::size_t user_group_subscription_count() const noexcept { return groups_.size(); }

private:
    struct ProviderEntry;
    struct GroupEntry;

    [[nodiscard]] async::DetachedTask watch_bt1(nacos::Subscription<nacos::ConfigData> subscription) noexcept;
    [[nodiscard]] async::DetachedTask watch_models(nacos::Subscription<nacos::ConfigData> subscription) noexcept;
    [[nodiscard]] async::DetachedTask watch_provider(std::shared_ptr<ProviderEntry> entry,
                                                     nacos::Subscription<nacos::ConfigData> subscription) noexcept;
    [[nodiscard]] async::DetachedTask watch_group(std::shared_ptr<GroupEntry> entry,
                                                  nacos::Subscription<nacos::ConfigData> subscription) noexcept;

    void apply_bt1(const nacos::ConfigData &data);
    void apply_models(const nacos::ConfigData &data);
    void apply_provider(const std::shared_ptr<ProviderEntry> &entry, const nacos::ConfigData &data);
    void apply_group(const std::shared_ptr<GroupEntry> &entry, const nacos::ConfigData &data);

    [[nodiscard]] std::expected<void, nacos::ConfigServiceError>
    reconcile_dependencies(const ModelsConfigSnapshot &models, std::unordered_set<std::string> &provider_names,
                           std::unordered_set<std::string> &group_names);
    [[nodiscard]] std::expected<void, nacos::ConfigServiceError> add_provider(std::string name);
    [[nodiscard]] std::expected<void, nacos::ConfigServiceError> add_group(std::string name);
    void remove_unreferenced(const std::unordered_set<std::string> &provider_names,
                             const std::unordered_set<std::string> &group_names);
    void stop_all_dynamic() noexcept;
    void rebuild_project();
    void report_failure(std::string_view data_id, std::string_view md5, LlmConfigError error);
    void report_not_found(std::string_view data_id);
    void task_done() noexcept;

    event::EventLoop *loop_ = nullptr;
    nacos::ConfigService *config_service_ = nullptr;
    LlmConfigManagerState state_ = LlmConfigManagerState::Created;
    async::WaitGroup tasks_;
    async::Watch<bool> stop_{false};
    std::optional<async::Watch<bool>::Publisher> stop_publisher_;
    std::map<std::string, std::shared_ptr<ProviderEntry>, std::less<>> providers_;
    std::map<std::string, std::shared_ptr<GroupEntry>, std::less<>> groups_;
    std::shared_ptr<const Bt1KeySnapshot> bt1_keys_;
    std::shared_ptr<const ModelsConfigSnapshot> models_;
    std::shared_ptr<const LlmProjectSnapshot> project_;
    std::optional<LlmConfigFailure> last_failure_;
    std::uint64_t project_generation_ = 0;
    std::uint64_t successful_updates_ = 0;
    std::uint64_t failed_updates_ = 0;
};

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_LLM_CONFIG_MANAGER_H
