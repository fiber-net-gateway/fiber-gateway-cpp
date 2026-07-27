#ifndef FIBER_AI_SERVER_AI_SERVER_RUNTIME_H
#define FIBER_AI_SERVER_AI_SERVER_RUNTIME_H

#include "AiServer.h"
#include "AiServerConfig.h"
#include "audit/LlmAuditWriter.h"
#include "config/LlmConfigManager.h"
#include "limit/RateLimitClusterMembership.h"

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
#include <fiber/cat/CatClient.h>
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
    CreateCatClient,
    CreateAuditWriter,
    AllocateRuntime,
    Bind,
    StartNacosClient,
    StartConfigService,
    StartNamingService,
    StartCatClient,
    StartConfigManager,
    StartRateLimitCluster,
    InitialConfigUnavailable,
    InitialConfigTimeout,
};

struct AiServerRuntimeError {
    AiServerRuntimeErrorCode code = AiServerRuntimeErrorCode::AllocateRuntime;
    common::IoErr io_error = common::IoErr::None;
    nacos::NacosCreateErrorCode create_error = nacos::NacosCreateErrorCode::InvalidState;
    nacos::ConfigServiceErrorCode config_error = nacos::ConfigServiceErrorCode::Protocol;
    nacos::NamingServiceErrorCode naming_error = nacos::NamingServiceErrorCode::Protocol;
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
           event::EventLoop &audit_loop, event::EventLoopGroup &http_workers, const AiServerConfig &config,
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

    struct ClusterStartStatus {
        bool success = false;
        AiServerRuntimeError error;
    };

    struct CatStartStatus {
        bool success = false;
        AiServerRuntimeError error;
    };

    AiServerRuntime(event::EventLoop &accept_loop, event::EventLoop &nacos_loop, event::EventLoop &cat_loop,
                    event::EventLoop &audit_loop, event::EventLoopGroup &http_workers,
                    net::SocketAddress listen_address, net::ListenOptions listen_options,
                    std::chrono::milliseconds initial_config_timeout, std::optional<net::IpAddress> advertise_address,
                    std::string service_name, std::string service_group, std::unique_ptr<cat::CatClient> cat_client,
                    std::unique_ptr<LlmAuditWriter> audit_writer, std::unique_ptr<nacos::NacosClient> nacos_client,
                    std::unique_ptr<nacos::ConfigService> config_service,
                    std::unique_ptr<nacos::NamingService> naming_service) noexcept;

    [[nodiscard]] async::DetachedTask start_nacos() noexcept;
    [[nodiscard]] async::DetachedTask start_cat() noexcept;
    [[nodiscard]] async::DetachedTask start_rate_limit_cluster(std::string advertise_ipv4, std::uint16_t port) noexcept;
    [[nodiscard]] async::DetachedTask shutdown_nacos() noexcept;
    [[nodiscard]] async::DetachedTask shutdown_cat() noexcept;
    [[nodiscard]] async::Task<void> stop_nacos() noexcept;
    [[nodiscard]] async::Task<void> stop_cat() noexcept;
    [[nodiscard]] async::Task<void> fail_start() noexcept;

    event::EventLoop *accept_loop_ = nullptr;
    event::EventLoop *nacos_loop_ = nullptr;
    event::EventLoop *cat_loop_ = nullptr;
    event::EventLoop *audit_loop_ = nullptr;
    net::SocketAddress listen_address_;
    net::ListenOptions listen_options_;
    std::chrono::milliseconds initial_config_timeout_{0};
    std::optional<net::IpAddress> advertise_address_;
    std::unique_ptr<cat::CatClient> cat_client_;
    std::unique_ptr<LlmAuditWriter> audit_writer_;
    std::unique_ptr<nacos::NacosClient> nacos_client_;
    std::unique_ptr<nacos::ConfigService> config_service_;
    std::unique_ptr<nacos::NamingService> naming_service_;
    LlmConfigManager config_manager_;
    AiServer server_;
    RateLimitClusterMembership rate_limit_membership_;
    async::WaitGroup nacos_start_tasks_;
    async::Watch<NacosStartStatus> nacos_start_status_;
    std::optional<async::Watch<NacosStartStatus>::Publisher> nacos_start_publisher_;
    async::WaitGroup cat_start_tasks_;
    async::Watch<CatStartStatus> cat_start_status_;
    std::optional<async::Watch<CatStartStatus>::Publisher> cat_start_publisher_;
    async::WaitGroup cluster_start_tasks_;
    async::Watch<ClusterStartStatus> cluster_start_status_;
    std::optional<async::Watch<ClusterStartStatus>::Publisher> cluster_start_publisher_;
    async::Watch<bool> nacos_stopped_{false};
    std::optional<async::Watch<bool>::Publisher> nacos_stopped_publisher_;
    async::Watch<bool> cat_stopped_{false};
    std::optional<async::Watch<bool>::Publisher> cat_stopped_publisher_;
    AiServerRuntimeState state_ = AiServerRuntimeState::Created;
    bool nacos_shutdown_spawned_ = false;
    bool cat_shutdown_spawned_ = false;
};

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_AI_SERVER_RUNTIME_H
