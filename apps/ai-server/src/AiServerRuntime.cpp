#include "AiServerRuntime.h"

#include <new>
#include <utility>

#include <async/Spawn.h>
#include <common/Assert.h>

namespace fiber::ai_server {
namespace {

AiServerRuntimeError create_error(AiServerRuntimeErrorCode code, nacos::NacosCreateError error) noexcept {
    return AiServerRuntimeError{
            .code = code,
            .create_error = error.code,
    };
}

AiServerRuntimeError io_error(AiServerRuntimeErrorCode code, common::IoErr error) noexcept {
    return AiServerRuntimeError{
            .code = code,
            .io_error = error,
    };
}

AiServerRuntimeError config_error(nacos::ConfigServiceError error) {
    return AiServerRuntimeError{
            .code = AiServerRuntimeErrorCode::StartConfigManager,
            .io_error = error.io_error,
            .config_error = error.code,
            .message = std::move(error.message),
    };
}

} // namespace

std::expected<std::unique_ptr<AiServerRuntime>, AiServerRuntimeError>
AiServerRuntime::create(event::EventLoop &loop, const AiServerConfig &config,
                        const net::ListenOptions &listen_options) {
    auto client = nacos::NacosClient::create(loop, config.nacos_config());
    if (!client) {
        return std::unexpected(create_error(AiServerRuntimeErrorCode::CreateNacosClient, client.error()));
    }
    auto service = nacos::ConfigService::create(**client);
    if (!service) {
        return std::unexpected(create_error(AiServerRuntimeErrorCode::CreateConfigService, service.error()));
    }

    auto runtime = std::unique_ptr<AiServerRuntime>(
            new (std::nothrow) AiServerRuntime(loop, std::move(*client), std::move(*service)));
    if (!runtime) {
        return std::unexpected(AiServerRuntimeError{
                .code = AiServerRuntimeErrorCode::AllocateRuntime,
                .create_error = nacos::NacosCreateErrorCode::NoMem,
        });
    }
    auto bound = runtime->server_.bind(config.listen_address(), listen_options);
    if (!bound) {
        return std::unexpected(io_error(AiServerRuntimeErrorCode::Bind, bound.error()));
    }
    return runtime;
}

AiServerRuntime::AiServerRuntime(event::EventLoop &loop, std::unique_ptr<nacos::NacosClient> nacos_client,
                                 std::unique_ptr<nacos::ConfigService> config_service) noexcept :
    loop_(&loop), nacos_client_(std::move(nacos_client)), config_service_(std::move(config_service)),
    config_manager_(loop, *config_service_), server_(loop, config_manager_) {
    FIBER_ASSERT(nacos_client_ != nullptr);
    FIBER_ASSERT(config_service_ != nullptr);
}

AiServerRuntime::~AiServerRuntime() {
    FIBER_ASSERT(state_ == AiServerRuntimeState::Created || state_ == AiServerRuntimeState::Stopped);
}

async::Task<std::expected<void, AiServerRuntimeError>> AiServerRuntime::start() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(state_ == AiServerRuntimeState::Created);

    auto client_started = nacos_client_->start();
    if (!client_started) {
        co_return std::unexpected(io_error(AiServerRuntimeErrorCode::StartNacosClient, client_started.error()));
    }
    auto service_started = config_service_->start();
    if (!service_started) {
        co_await nacos_client_->shutdown();
        state_ = AiServerRuntimeState::Stopped;
        co_return std::unexpected(io_error(AiServerRuntimeErrorCode::StartConfigService, service_started.error()));
    }
    auto manager_started = config_manager_.start();
    if (!manager_started) {
        co_await config_service_->shutdown();
        co_await nacos_client_->shutdown();
        state_ = AiServerRuntimeState::Stopped;
        co_return std::unexpected(config_error(std::move(manager_started.error())));
    }

    state_ = AiServerRuntimeState::Running;
    async::spawn(*loop_, [this]() { return server_.serve(); });
    co_return std::expected<void, AiServerRuntimeError>{};
}

async::Task<void> AiServerRuntime::shutdown() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (state_ == AiServerRuntimeState::Stopped) {
        co_return;
    }
    if (state_ == AiServerRuntimeState::Created) {
        server_.close();
        co_await config_manager_.shutdown();
        co_await config_service_->shutdown();
        co_await nacos_client_->shutdown();
        state_ = AiServerRuntimeState::Stopped;
        co_return;
    }

    state_ = AiServerRuntimeState::Stopping;
    co_await server_.shutdown_and_wait();
    co_await config_manager_.shutdown();
    co_await config_service_->shutdown();
    co_await nacos_client_->shutdown();
    state_ = AiServerRuntimeState::Stopped;
}

} // namespace fiber::ai_server
