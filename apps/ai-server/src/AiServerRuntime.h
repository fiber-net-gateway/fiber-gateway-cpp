#ifndef FIBER_AI_SERVER_AI_SERVER_RUNTIME_H
#define FIBER_AI_SERVER_AI_SERVER_RUNTIME_H

#include "AiServer.h"
#include "AiServerConfig.h"
#include "config/LlmConfigManager.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>

#include <async/Task.h>
#include <async/WaitGroup.h>
#include <async/Watch.h>
#include <common/IoError.h>
#include <common/NonCopyable.h>
#include <common/NonMovable.h>
#include <event/EventLoop.h>
#include <event/EventLoopGroup.h>
#include <fiber/nacos/ConfigService.h>
#include <fiber/nacos/NacosClient.h>
#include <fiber/nacos/NacosCreateError.h>
#include <fiber/nacos/NamingService.h>
#include <net/TcpListener.h>

namespace fiber::ai_server {

[[nodiscard]] std::size_t default_http_worker_count() noexcept;

enum class AiServerRuntimeErrorCode : std::uint8_t {
    CreateNacosClient,
    CreateConfigService,
    CreateNamingService,
    AllocateRuntime,
    Bind,
    StartNacosClient,
    StartConfigService,
    StartNamingService,
    StartConfigManager,
    InitialConfigUnavailable,
    InitialConfigTimeout,
};

struct AiServerRuntimeError {
    AiServerRuntimeErrorCode code = AiServerRuntimeErrorCode::AllocateRuntime;
    common::IoErr io_error = common::IoErr::None;
    nacos::NacosCreateErrorCode create_error = nacos::NacosCreateErrorCode::InvalidState;
    nacos::ConfigServiceErrorCode config_error = nacos::ConfigServiceErrorCode::Protocol;
    std::string message;
};

enum class AiServerRuntimeState : std::uint8_t {
    Created,
    Starting,
    Running,
    Stopping,
    Stopped,
};

class AiServerRuntime final : public common::NonCopyable, public common::NonMovable {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<AiServerRuntime>, AiServerRuntimeError>
    create(event::EventLoop &accept_loop, event::EventLoop &nacos_loop, event::EventLoop &cat_loop,
           event::EventLoopGroup &http_workers, const AiServerConfig &config,
           const net::ListenOptions &listen_options = {});

    ~AiServerRuntime();

    [[nodiscard]] async::Task<std::expected<void, AiServerRuntimeError>> start() noexcept;
    [[nodiscard]] async::Task<void> shutdown() noexcept;

    [[nodiscard]] AiServerRuntimeState state() const noexcept { return state_; }
    [[nodiscard]] int fd() const noexcept { return server_.fd(); }

private:
    struct NacosStartStatus {
        bool success = false;
        AiServerRuntimeError error;
    };

    AiServerRuntime(event::EventLoop &accept_loop, event::EventLoop &nacos_loop, event::EventLoop &cat_loop,
                    event::EventLoopGroup &http_workers, net::SocketAddress listen_address,
                    net::ListenOptions listen_options, std::chrono::milliseconds initial_config_timeout,
                    std::unique_ptr<nacos::NacosClient> nacos_client,
                    std::unique_ptr<nacos::ConfigService> config_service,
                    std::unique_ptr<nacos::NamingService> naming_service) noexcept;

    [[nodiscard]] async::DetachedTask start_nacos() noexcept;
    [[nodiscard]] async::DetachedTask shutdown_nacos() noexcept;
    [[nodiscard]] async::Task<void> stop_nacos() noexcept;
    [[nodiscard]] async::Task<void> fail_start() noexcept;

    event::EventLoop *accept_loop_ = nullptr;
    event::EventLoop *nacos_loop_ = nullptr;
    event::EventLoop *cat_loop_ = nullptr;
    net::SocketAddress listen_address_;
    net::ListenOptions listen_options_;
    std::chrono::milliseconds initial_config_timeout_{0};
    std::unique_ptr<nacos::NacosClient> nacos_client_;
    std::unique_ptr<nacos::ConfigService> config_service_;
    std::unique_ptr<nacos::NamingService> naming_service_;
    LlmConfigManager config_manager_;
    async::WaitGroup nacos_start_tasks_;
    async::Watch<NacosStartStatus> nacos_start_status_;
    std::optional<async::Watch<NacosStartStatus>::Publisher> nacos_start_publisher_;
    async::Watch<bool> nacos_stopped_{false};
    std::optional<async::Watch<bool>::Publisher> nacos_stopped_publisher_;
    AiServer server_;
    AiServerRuntimeState state_ = AiServerRuntimeState::Created;
    bool nacos_shutdown_spawned_ = false;
};

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_AI_SERVER_RUNTIME_H
