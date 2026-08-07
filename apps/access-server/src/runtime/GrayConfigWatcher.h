#ifndef FIBER_ACCESS_SERVER_GRAY_CONFIG_WATCHER_H
#define FIBER_ACCESS_SERVER_GRAY_CONFIG_WATCHER_H

#include "GrayMatchStore.h"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>

#include <fiber/async/Task.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/event/EventLoop.h>
#include <fiber/nacos/ConfigService.h>

namespace fiber::access_server {

enum class GrayConfigWatcherState : std::uint8_t {
    Created,
    Running,
    Stopping,
    Stopped,
};

struct GrayConfigWatcherOptions {
    std::string data_id = std::string(kGrayConfigDataId);
    std::string group = std::string(kDefaultNacosGroup);
};

struct GrayConfigWatcherFailure {
    std::string md5;
    AccessConfigError error;
};

class GrayConfigWatcher final : public common::NonCopyable, public common::NonMovable {
public:
    GrayConfigWatcher(event::EventLoop &loop, nacos::ConfigService &config_service, GrayMatchStore &store,
                      GrayConfigWatcherOptions options = {});
    ~GrayConfigWatcher();

    [[nodiscard]] std::expected<void, nacos::ConfigServiceError> start();
    [[nodiscard]] async::Task<void> shutdown() noexcept;

    [[nodiscard]] GrayConfigWatcherState state() const noexcept { return state_; }
    [[nodiscard]] std::uint64_t successful_updates() const noexcept { return successful_updates_; }
    [[nodiscard]] std::uint64_t failed_updates() const noexcept { return failed_updates_; }
    [[nodiscard]] const std::optional<GrayConfigWatcherFailure> &last_failure() const noexcept { return last_failure_; }

private:
    static void on_notify(void *context, const nacos::SubscriptionResult<nacos::ConfigData> &result) noexcept;
    void apply(const nacos::ConfigData &data);
    void request_stop() noexcept;

    event::EventLoop *loop_ = nullptr;
    nacos::ConfigService *config_service_ = nullptr;
    GrayMatchStore *store_ = nullptr;
    GrayConfigWatcherOptions options_;
    std::optional<nacos::Subscription<nacos::ConfigData>> subscription_;
    std::optional<GrayConfigWatcherFailure> last_failure_;
    GrayConfigWatcherState state_ = GrayConfigWatcherState::Created;
    std::uint64_t successful_updates_ = 0;
    std::uint64_t failed_updates_ = 0;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_GRAY_CONFIG_WATCHER_H
