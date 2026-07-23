#ifndef FIBER_AI_SERVER_LLM_CONFIG_MANAGER_H
#define FIBER_AI_SERVER_LLM_CONFIG_MANAGER_H

#include "LlmConfigCodec.h"
#include "LlmConfigSnapshot.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>

#include <async/Task.h>
#include <async/Watch.h>
#include <common/NonCopyable.h>
#include <common/NonMovable.h>
#include <event/EventLoop.h>
#include <fiber/nacos/ConfigService.h>
#include <fiber/nacos/NamingService.h>

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
    using SnapshotSubscriber = async::Watch<LlmConfigSnapshot>::Subscriber;

    LlmConfigManager(event::EventLoop &loop, nacos::ConfigService &config_service,
                     nacos::NamingService &naming_service);
    ~LlmConfigManager();

    [[nodiscard]] std::expected<void, nacos::ConfigServiceError> start();
    [[nodiscard]] async::Task<void> shutdown() noexcept;

    [[nodiscard]] LlmConfigManagerState state() const noexcept;
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] SnapshotSubscriber subscribe_snapshot();
    [[nodiscard]] std::shared_ptr<const Bt1KeySnapshot> current_bt1_keys() const noexcept;
    [[nodiscard]] std::shared_ptr<const LlmProjectSnapshot> current_project() const noexcept;
    [[nodiscard]] const std::optional<LlmConfigFailure> &last_failure() const noexcept;
    [[nodiscard]] std::uint64_t successful_updates() const noexcept;
    [[nodiscard]] std::uint64_t failed_updates() const noexcept;
    [[nodiscard]] std::size_t provider_subscription_count() const noexcept;
    [[nodiscard]] std::size_t user_group_subscription_count() const noexcept;
    [[nodiscard]] std::size_t service_subscription_count() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_LLM_CONFIG_MANAGER_H
