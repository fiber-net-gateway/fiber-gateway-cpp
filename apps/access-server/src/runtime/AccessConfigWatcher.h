#ifndef FIBER_ACCESS_SERVER_ACCESS_CONFIG_WATCHER_H
#define FIBER_ACCESS_SERVER_ACCESS_CONFIG_WATCHER_H

#include "RouteConfigStore.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <fiber/async/Spawn.h>
#include <fiber/async/Task.h>
#include <fiber/async/WaitGroup.h>
#include <fiber/async/Watch.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/event/EventLoop.h>
#include <fiber/nacos/ConfigService.h>

namespace fiber::access_server {

enum class AccessConfigWatcherState : std::uint8_t {
    Created,
    Running,
    Stopping,
    Stopped,
};

struct AccessConfigWatcherOptions {
    std::string project_list_data_id = std::string(kProjectListDataId);
    std::string project_route_data_id_prefix = std::string(kProjectRouteDataIdPrefix);
    std::string project_route_group = std::string(kProjectRouteGroup);
};

struct AccessConfigWatcherFailure {
    std::string data_id;
    std::string md5;
    AccessConfigError error;
};

struct RouteSnapshotObserver {
    using Function = void (*)(void *context, std::shared_ptr<const AccessRouteSnapshot> snapshot) noexcept;

    void *context = nullptr;
    Function on_update = nullptr;
};

// Owns the Java-compatible two-level Nacos subscription graph:
// project-list -> one route-config subscription per listed project.
//
// start(), shutdown(), and all subscription mutations are owner-loop-only.
// RouteConfigStore publishes immutable snapshots for request workers.
class AccessConfigWatcher final : public common::NonCopyable, public common::NonMovable {
public:
    AccessConfigWatcher(event::EventLoop &loop, nacos::ConfigService &config_service, RouteConfigStore &store,
                        AccessConfigWatcherOptions options = {}, RouteSnapshotObserver observer = {});
    ~AccessConfigWatcher();

    [[nodiscard]] std::expected<void, nacos::ConfigServiceError> start();
    [[nodiscard]] async::Task<void> shutdown() noexcept;
    [[nodiscard]] async::Watch<bool>::Subscriber subscribe_ready() { return ready_.subscribe(); }

    [[nodiscard]] AccessConfigWatcherState state() const noexcept { return state_; }
    [[nodiscard]] bool initial_project_list_received() const noexcept { return initial_project_list_received_; }
    [[nodiscard]] std::size_t project_subscription_count() const noexcept { return projects_.size(); }
    [[nodiscard]] std::uint64_t successful_updates() const noexcept { return successful_updates_; }
    [[nodiscard]] std::uint64_t failed_updates() const noexcept { return failed_updates_; }
    [[nodiscard]] const std::optional<AccessConfigWatcherFailure> &last_failure() const noexcept {
        return last_failure_;
    }

private:
    struct ProjectListEntry;
    struct ProjectEntry;

    static void project_list_notify(void *context, const nacos::SubscriptionResult<nacos::ConfigData> &result) noexcept;
    static void project_notify(void *context, const nacos::SubscriptionResult<nacos::ConfigData> &result) noexcept;

    void apply_project_list(const nacos::ConfigData &data);
    void apply_project(const std::shared_ptr<ProjectEntry> &entry, const nacos::ConfigData &data);
    [[nodiscard]] async::DetachedTask apply_ready_project(std::shared_ptr<ProjectEntry> entry,
                                                          PreparedConfigUpdate prepared, std::uint64_t generation,
                                                          std::uint64_t revision_version, std::string data_id,
                                                          std::string md5) noexcept;
    void reconcile_projects(std::string_view content);
    void add_project(std::string project);
    void remove_project(std::string_view project);
    void publish_observer(const std::shared_ptr<const AccessRouteSnapshot> &snapshot) const noexcept;
    void report_failure(std::string data_id, std::string md5, AccessConfigError error);

    event::EventLoop *loop_ = nullptr;
    nacos::ConfigService *config_service_ = nullptr;
    RouteConfigStore *store_ = nullptr;
    AccessConfigWatcherOptions options_;
    RouteSnapshotObserver observer_;
    std::unique_ptr<ProjectListEntry> project_list_;
    std::map<std::string, std::shared_ptr<ProjectEntry>, std::less<>> projects_;
    std::optional<AccessConfigWatcherFailure> last_failure_;
    async::Watch<bool> ready_{false};
    std::optional<async::Watch<bool>::Publisher> ready_publisher_;
    async::WaitGroup readiness_tasks_;
    AccessConfigWatcherState state_ = AccessConfigWatcherState::Created;
    bool initial_project_list_received_ = false;
    std::uint64_t successful_updates_ = 0;
    std::uint64_t failed_updates_ = 0;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_CONFIG_WATCHER_H
