#ifndef FIBER_AI_SERVER_AI_SERVER_RUNTIME_H
#define FIBER_AI_SERVER_AI_SERVER_RUNTIME_H

#include "AiServer.h"
#include "AiServerConfig.h"
#include "config/LlmConfigManager.h"

#include <cstdint>
#include <expected>
#include <memory>
#include <string>

#include <async/Task.h>
#include <common/IoError.h>
#include <common/NonCopyable.h>
#include <common/NonMovable.h>
#include <event/EventLoop.h>
#include <fiber/nacos/ConfigService.h>
#include <fiber/nacos/NacosClient.h>
#include <fiber/nacos/NacosCreateError.h>
#include <net/TcpListener.h>

namespace fiber::ai_server {

enum class AiServerRuntimeErrorCode : std::uint8_t {
    CreateNacosClient,
    CreateConfigService,
    AllocateRuntime,
    Bind,
    StartNacosClient,
    StartConfigService,
    StartConfigManager,
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
    Running,
    Stopping,
    Stopped,
};

class AiServerRuntime final : public common::NonCopyable, public common::NonMovable {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<AiServerRuntime>, AiServerRuntimeError>
    create(event::EventLoop &loop, const AiServerConfig &config, const net::ListenOptions &listen_options = {});

    ~AiServerRuntime();

    [[nodiscard]] async::Task<std::expected<void, AiServerRuntimeError>> start() noexcept;
    [[nodiscard]] async::Task<void> shutdown() noexcept;

    [[nodiscard]] AiServerRuntimeState state() const noexcept { return state_; }
    [[nodiscard]] int fd() const noexcept { return server_.fd(); }
    [[nodiscard]] const LlmConfigManager &config_manager() const noexcept { return config_manager_; }

private:
    AiServerRuntime(event::EventLoop &loop, std::unique_ptr<nacos::NacosClient> nacos_client,
                    std::unique_ptr<nacos::ConfigService> config_service) noexcept;

    event::EventLoop *loop_ = nullptr;
    std::unique_ptr<nacos::NacosClient> nacos_client_;
    std::unique_ptr<nacos::ConfigService> config_service_;
    LlmConfigManager config_manager_;
    AiServer server_;
    AiServerRuntimeState state_ = AiServerRuntimeState::Created;
};

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_AI_SERVER_RUNTIME_H
