#include "AiServerRuntime.h"

#include <thread>

#include <new>
#include <utility>

#ifdef __linux__
#include <sched.h>
#endif

#include <async/Sleep.h>
#include <async/Spawn.h>
#include <async/TaskSelect.h>
#include <async/WhenAny.h>
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

std::size_t default_http_worker_count() noexcept {
#ifdef __linux__
    cpu_set_t affinity;
    CPU_ZERO(&affinity);
    if (::sched_getaffinity(0, sizeof(affinity), &affinity) == 0) {
        const int count = CPU_COUNT(&affinity);
        if (count > 0) {
            return static_cast<std::size_t>(count);
        }
    }
#endif
    const unsigned int count = std::thread::hardware_concurrency();
    return count == 0 ? 1 : static_cast<std::size_t>(count);
}

std::expected<std::unique_ptr<AiServerRuntime>, AiServerRuntimeError>
AiServerRuntime::create(event::EventLoop &accept_loop, event::EventLoop &nacos_loop, event::EventLoop &cat_loop,
                        event::EventLoopGroup &http_workers, const AiServerConfig &config,
                        const net::ListenOptions &listen_options) {
    auto client = nacos::NacosClient::create(nacos_loop, config.nacos_config());
    if (!client) {
        return std::unexpected(create_error(AiServerRuntimeErrorCode::CreateNacosClient, client.error()));
    }
    auto service = nacos::ConfigService::create(**client);
    if (!service) {
        return std::unexpected(create_error(AiServerRuntimeErrorCode::CreateConfigService, service.error()));
    }

    auto runtime = std::unique_ptr<AiServerRuntime>(new (std::nothrow) AiServerRuntime(
            accept_loop, nacos_loop, cat_loop, http_workers, config.listen_address(), listen_options,
            config.initial_config_timeout(), std::move(*client), std::move(*service)));
    if (!runtime) {
        return std::unexpected(AiServerRuntimeError{
                .code = AiServerRuntimeErrorCode::AllocateRuntime,
                .create_error = nacos::NacosCreateErrorCode::NoMem,
        });
    }
    return runtime;
}

AiServerRuntime::AiServerRuntime(event::EventLoop &accept_loop, event::EventLoop &nacos_loop,
                                 event::EventLoop &cat_loop, event::EventLoopGroup &http_workers,
                                 net::SocketAddress listen_address, net::ListenOptions listen_options,
                                 std::chrono::milliseconds initial_config_timeout,
                                 std::unique_ptr<nacos::NacosClient> nacos_client,
                                 std::unique_ptr<nacos::ConfigService> config_service) noexcept :
    accept_loop_(&accept_loop), nacos_loop_(&nacos_loop), cat_loop_(&cat_loop),
    listen_address_(std::move(listen_address)), listen_options_(std::move(listen_options)),
    initial_config_timeout_(initial_config_timeout), nacos_client_(std::move(nacos_client)),
    config_service_(std::move(config_service)), config_manager_(nacos_loop, *config_service_),
    server_(accept_loop, http_workers) {
    FIBER_ASSERT(nacos_client_ != nullptr);
    FIBER_ASSERT(config_service_ != nullptr);
    FIBER_ASSERT(accept_loop_ != nacos_loop_);
    FIBER_ASSERT(accept_loop_ != cat_loop_);
    FIBER_ASSERT(nacos_loop_ != cat_loop_);
    for (std::size_t i = 0; i < http_workers.size(); ++i) {
        FIBER_ASSERT(&http_workers.at(i) != nacos_loop_);
        FIBER_ASSERT(&http_workers.at(i) != cat_loop_);
    }
    nacos_start_publisher_ = nacos_start_status_.acquire_publisher();
    FIBER_ASSERT(nacos_start_publisher_.has_value());
    nacos_stopped_publisher_ = nacos_stopped_.acquire_publisher();
    FIBER_ASSERT(nacos_stopped_publisher_.has_value());
}

AiServerRuntime::~AiServerRuntime() {
    FIBER_ASSERT(state_ == AiServerRuntimeState::Created || state_ == AiServerRuntimeState::Stopped);
    FIBER_ASSERT(nacos_start_tasks_.empty());
}

async::DetachedTask AiServerRuntime::start_nacos() noexcept {
    FIBER_ASSERT(nacos_loop_->in_loop());
    auto client_started = nacos_client_->start();
    if (!client_started) {
        nacos_start_publisher_->publish(NacosStartStatus{
                .error = io_error(AiServerRuntimeErrorCode::StartNacosClient, client_started.error()),
        });
        nacos_start_tasks_.done();
        co_return;
    }
    auto service_started = config_service_->start();
    if (!service_started) {
        co_await nacos_client_->shutdown();
        nacos_start_publisher_->publish(NacosStartStatus{
                .error = io_error(AiServerRuntimeErrorCode::StartConfigService, service_started.error()),
        });
        nacos_start_tasks_.done();
        co_return;
    }
    auto manager_started = config_manager_.start();
    if (!manager_started) {
        co_await config_service_->shutdown();
        co_await nacos_client_->shutdown();
        nacos_start_publisher_->publish(NacosStartStatus{
                .error = config_error(std::move(manager_started.error())),
        });
        nacos_start_tasks_.done();
        co_return;
    }
    nacos_start_publisher_->publish(NacosStartStatus{.success = true});
    nacos_start_tasks_.done();
}

async::DetachedTask AiServerRuntime::shutdown_nacos() noexcept {
    FIBER_ASSERT(nacos_loop_->in_loop());
    co_await nacos_start_tasks_.join();
    co_await config_manager_.shutdown();
    co_await config_service_->shutdown();
    co_await nacos_client_->shutdown();
    nacos_stopped_publisher_->publish(true);
}

async::Task<void> AiServerRuntime::stop_nacos() noexcept {
    FIBER_ASSERT(accept_loop_->in_loop());
    auto stopped = nacos_stopped_.subscribe();
    auto snapshot = stopped.current();
    if (!nacos_shutdown_spawned_) {
        nacos_shutdown_spawned_ = true;
        async::spawn(*nacos_loop_, [this]() { return shutdown_nacos(); });
    }
    while (!snapshot.value || !*snapshot.value) {
        snapshot = co_await stopped.next(snapshot.version);
    }
}

async::Task<void> AiServerRuntime::fail_start() noexcept {
    FIBER_ASSERT(accept_loop_->in_loop());
    state_ = AiServerRuntimeState::Stopping;
    co_await server_.shutdown_and_wait();
    co_await stop_nacos();
    state_ = AiServerRuntimeState::Stopped;
}

async::Task<std::expected<void, AiServerRuntimeError>> AiServerRuntime::start() noexcept {
    FIBER_ASSERT(accept_loop_->in_loop());
    FIBER_ASSERT(state_ == AiServerRuntimeState::Created);
    state_ = AiServerRuntimeState::Starting;

    auto nacos_status = nacos_start_status_.subscribe();
    auto nacos_snapshot = nacos_status.current();
    nacos_start_tasks_.add();
    async::spawn(*nacos_loop_, [this]() { return start_nacos(); });
    while (!nacos_snapshot.value) {
        nacos_snapshot = co_await nacos_status.next(nacos_snapshot.version);
    }
    if (!nacos_snapshot.value->success) {
        AiServerRuntimeError error = nacos_snapshot.value->error;
        co_await fail_start();
        co_return std::unexpected(std::move(error));
    }

    bool workers_ready = false;
    if (initial_config_timeout_ > std::chrono::milliseconds::zero()) {
        auto ready_or_timeout =
                co_await async::when_any([this]() { return server_.start_config_workers(config_manager_).select(); },
                                         [timeout = initial_config_timeout_]() { return async::sleep(timeout); });
        if (ready_or_timeout.is<1>()) {
            ready_or_timeout.get<1>();
            co_await fail_start();
            co_return std::unexpected(AiServerRuntimeError{
                    .code = AiServerRuntimeErrorCode::InitialConfigTimeout,
                    .io_error = common::IoErr::TimedOut,
                    .message = "initial Nacos LLM configuration sync timed out",
            });
        }
        workers_ready = std::move(ready_or_timeout).get<0>();
    } else {
        workers_ready = co_await server_.start_config_workers(config_manager_);
    }
    if (!workers_ready) {
        co_await fail_start();
        co_return std::unexpected(AiServerRuntimeError{
                .code = AiServerRuntimeErrorCode::InitialConfigUnavailable,
                .io_error = common::IoErr::Canceled,
                .message = "initial Nacos LLM configuration sync stopped",
        });
    }

    auto bound = server_.bind(listen_address_, listen_options_);
    if (!bound) {
        const common::IoErr error = bound.error();
        co_await fail_start();
        co_return std::unexpected(io_error(AiServerRuntimeErrorCode::Bind, error));
    }
    state_ = AiServerRuntimeState::Running;
    async::spawn(*accept_loop_, [this]() { return server_.serve(); });
    co_return std::expected<void, AiServerRuntimeError>{};
}

async::Task<void> AiServerRuntime::shutdown() noexcept {
    FIBER_ASSERT(accept_loop_->in_loop());
    if (state_ == AiServerRuntimeState::Stopped) {
        co_return;
    }
    if (state_ == AiServerRuntimeState::Created) {
        server_.close();
        co_await stop_nacos();
        state_ = AiServerRuntimeState::Stopped;
        co_return;
    }

    state_ = AiServerRuntimeState::Stopping;
    co_await server_.shutdown_and_wait();
    co_await stop_nacos();
    state_ = AiServerRuntimeState::Stopped;
}

} // namespace fiber::ai_server
