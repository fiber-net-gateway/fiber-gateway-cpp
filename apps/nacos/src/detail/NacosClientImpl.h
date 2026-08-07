#ifndef FIBER_NACOS_DETAIL_NACOS_CLIENT_IMPL_H
#define FIBER_NACOS_DETAIL_NACOS_CLIENT_IMPL_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <fiber/async/Spawn.h>
#include <fiber/async/Task.h>
#include <fiber/async/WaitGroup.h>
#include <fiber/async/Watch.h>
#include <fiber/common/IoError.h>
#include <fiber/event/EventLoop.h>
#include <fiber/nacos/NacosClient.h>
#include <fiber/nacos/NacosClientConfig.h>

#include "NacosServiceDependencies.h"

namespace fiber::nacos::detail {

enum class NacosClientState : std::uint8_t {
    Created,
    Running,
    Stopping,
    Stopped,
};

class NacosClientImpl {
public:
    NacosClientImpl(event::EventLoop &loop, NacosClientConfig config, NacosClientOptions options);
    ~NacosClientImpl();

    [[nodiscard]] static bool valid_options(const NacosClientOptions &options) noexcept;

    [[nodiscard]] common::IoResult<void> start() noexcept;
    [[nodiscard]] async::Task<void> shutdown() noexcept;

    [[nodiscard]] NacosAuthSubscriber subscribe_auth();
    [[nodiscard]] std::expected<NacosServiceDependencies, NacosCreateError> service_dependencies();

    [[nodiscard]] event::EventLoop &loop() const noexcept { return *loop_; }
    [[nodiscard]] const NacosClientConfig &config() const noexcept { return *config_; }

private:
    struct AuthLoginSuccess {
        std::string access_token;
        std::int64_t token_ttl = 0;
    };

    [[nodiscard]] async::DetachedTask run_auth() noexcept;
    [[nodiscard]] async::Task<std::expected<AuthLoginSuccess, common::IoErr>>
    login(std::size_t server_index, std::string_view target, std::string_view auth_body) noexcept;

    [[nodiscard]] async::Watch<bool>::Subscriber subscribe_shutdown();
    [[nodiscard]] bool running() const noexcept { return state_ == NacosClientState::Running; }
    void end_task() noexcept;
    void publish_auth(NacosAuthAccess auth_access);

    event::EventLoop *loop_ = nullptr;
    std::shared_ptr<const NacosClientConfig> config_;
    NacosClientOptions options_;
    NacosClientState state_ = NacosClientState::Created;
    async::WaitGroup task_group_;
    async::Watch<bool> shutdown_watch_{false};
    std::optional<async::Watch<bool>::Publisher> shutdown_publisher_;
    async::Watch<NacosAuthAccess> auth_watch_;
    std::optional<async::Watch<NacosAuthAccess>::Publisher> auth_publisher_;
};

} // namespace fiber::nacos::detail

#endif // FIBER_NACOS_DETAIL_NACOS_CLIENT_IMPL_H
