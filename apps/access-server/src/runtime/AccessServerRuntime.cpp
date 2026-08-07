#include "AccessServerRuntime.h"

#include <new>
#include <utility>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/async/TaskSelect.h>
#include <fiber/async/WhenAny.h>
#include <fiber/common/Assert.h>

namespace fiber::access_server {

std::string_view access_server_runtime_stage_name(AccessServerRuntimeErrorCode code) noexcept {
    switch (code) {
        case AccessServerRuntimeErrorCode::CreateNacosClient:
            return "create Nacos client";
        case AccessServerRuntimeErrorCode::CreateConfigService:
            return "create Nacos config service";
        case AccessServerRuntimeErrorCode::CreateNamingService:
            return "create Nacos naming service";
        case AccessServerRuntimeErrorCode::CreateCatClient:
            return "create CAT client";
        case AccessServerRuntimeErrorCode::AllocateRuntime:
            return "allocate access-server runtime";
        case AccessServerRuntimeErrorCode::InitializeWorkers:
            return "initialize HTTP worker resources";
        case AccessServerRuntimeErrorCode::StartNacosClient:
            return "start Nacos client";
        case AccessServerRuntimeErrorCode::StartConfigService:
            return "start Nacos config service";
        case AccessServerRuntimeErrorCode::StartNamingService:
            return "start Nacos naming service";
        case AccessServerRuntimeErrorCode::StartCatClient:
            return "start CAT client";
        case AccessServerRuntimeErrorCode::StartGrayWatcher:
            return "subscribe gray configuration";
        case AccessServerRuntimeErrorCode::StartAccessWatcher:
            return "subscribe access configuration";
        case AccessServerRuntimeErrorCode::InitialConfigUnavailable:
            return "receive initial project list";
        case AccessServerRuntimeErrorCode::InitialConfigTimeout:
            return "wait for initial project list";
        case AccessServerRuntimeErrorCode::Bind:
            return "bind HTTP listener";
        case AccessServerRuntimeErrorCode::BindMetrics:
            return "bind Prometheus listener";
    }
    return "start access-server";
}

AccessServerRuntimeError AccessServerRuntime::make_create_error(AccessServerRuntimeErrorCode code,
                                                                nacos::NacosCreateError error) noexcept {
    return AccessServerRuntimeError{
            .code = code,
            .create_error = error.code,
    };
}

AccessServerRuntimeError AccessServerRuntime::make_io_error(AccessServerRuntimeErrorCode code, common::IoErr error,
                                                            std::string message) {
    return AccessServerRuntimeError{
            .code = code,
            .io_error = error,
            .message = std::move(message),
    };
}

std::expected<std::unique_ptr<AccessServerRuntime>, AccessServerRuntimeError>
AccessServerRuntime::create(event::EventLoop &accept_loop, event::EventLoop &nacos_loop, event::EventLoop &cat_loop,
                            event::EventLoopGroup &http_workers, const AccessServerConfig &config,
                            const net::ListenOptions &listen_options) {
    auto client = nacos::NacosClient::create(nacos_loop, config.nacos_config());
    if (!client) {
        return std::unexpected(make_create_error(AccessServerRuntimeErrorCode::CreateNacosClient, client.error()));
    }
    auto config_service = nacos::ConfigService::create(**client);
    if (!config_service) {
        return std::unexpected(
                make_create_error(AccessServerRuntimeErrorCode::CreateConfigService, config_service.error()));
    }
    auto naming_service = nacos::NamingService::create(**client);
    if (!naming_service) {
        return std::unexpected(
                make_create_error(AccessServerRuntimeErrorCode::CreateNamingService, naming_service.error()));
    }
    std::unique_ptr<cat::CatClient> cat_client;
    if (config.cat_config()) {
        auto created = cat::CatClient::create(cat_loop, *config.cat_config());
        if (!created) {
            return std::unexpected(AccessServerRuntimeError{
                    .code = AccessServerRuntimeErrorCode::CreateCatClient,
                    .io_error = common::IoErr::Invalid,
                    .message = "failed to create CAT client",
            });
        }
        cat_client = std::move(*created);
    }

    auto runtime = std::unique_ptr<AccessServerRuntime>(new (std::nothrow) AccessServerRuntime(
            accept_loop, nacos_loop, cat_loop, http_workers, config.listen_address(), config.metrics_listen_address(),
            listen_options, config.initial_config_timeout(), config.default_max_request_body_size(), config.test_mode(),
            config.watcher_options(), config.gray_watcher_options(), config.service_discovery_options(),
            std::move(cat_client), std::move(*client), std::move(*config_service), std::move(*naming_service)));
    if (!runtime) {
        return std::unexpected(AccessServerRuntimeError{
                .code = AccessServerRuntimeErrorCode::AllocateRuntime,
                .create_error = nacos::NacosCreateErrorCode::NoMem,
        });
    }
    return runtime;
}

AccessServerRuntime::AccessServerRuntime(
        event::EventLoop &accept_loop, event::EventLoop &nacos_loop, event::EventLoop &cat_loop,
        event::EventLoopGroup &http_workers, net::SocketAddress listen_address,
        net::SocketAddress metrics_listen_address, net::ListenOptions listen_options,
        std::chrono::milliseconds initial_config_timeout, std::size_t default_max_request_body_size, bool test_mode,
        AccessConfigWatcherOptions watcher_options, GrayConfigWatcherOptions gray_options,
        AccessServiceDiscoveryOptions service_discovery_options, std::unique_ptr<cat::CatClient> cat_client,
        std::unique_ptr<nacos::NacosClient> nacos_client, std::unique_ptr<nacos::ConfigService> config_service,
        std::unique_ptr<nacos::NamingService> naming_service) noexcept :
    accept_loop_(&accept_loop), nacos_loop_(&nacos_loop), cat_loop_(&cat_loop),
    listen_address_(std::move(listen_address)), metrics_listen_address_(std::move(metrics_listen_address)),
    listen_options_(std::move(listen_options)), initial_config_timeout_(initial_config_timeout),
    cat_client_(std::move(cat_client)), nacos_client_(std::move(nacos_client)),
    config_service_(std::move(config_service)), naming_service_(std::move(naming_service)),
    service_discovery_(nacos_loop, *naming_service_,
                       AccessServiceOps{.swrr_options = service_discovery_options.swrr_options,
                                        .zone = service_discovery_options.zone}),
    route_store_(script_runtime_.compiler_adapter(), service_discovery_, std::move(service_discovery_options)),
    config_watcher_(nacos_loop, *config_service_, route_store_, std::move(watcher_options)),
    gray_watcher_(nacos_loop, *config_service_, gray_store_, std::move(gray_options)),
    server_(accept_loop, http_workers, route_store_, gray_store_.adapter(),
            AccessServerOptions{
                    .default_max_request_body_size = default_max_request_body_size,
                    .script_adapter = script_runtime_.request_adapter(),
                    .cat_client = cat_client_.get(),
                    .test_mode = test_mode,
            }) {
    FIBER_ASSERT(accept_loop_ != nacos_loop_);
    FIBER_ASSERT(accept_loop_ != cat_loop_);
    FIBER_ASSERT(nacos_loop_ != cat_loop_);
    for (std::size_t i = 0; i < http_workers.size(); ++i) {
        FIBER_ASSERT(&http_workers.at(i) != nacos_loop_);
        FIBER_ASSERT(&http_workers.at(i) != accept_loop_);
        FIBER_ASSERT(&http_workers.at(i) != cat_loop_);
    }
    nacos_start_publisher_ = nacos_start_status_.acquire_publisher();
    FIBER_ASSERT(nacos_start_publisher_.has_value());
    nacos_stopped_publisher_ = nacos_stopped_.acquire_publisher();
    FIBER_ASSERT(nacos_stopped_publisher_.has_value());
    cat_start_publisher_ = cat_start_status_.acquire_publisher();
    FIBER_ASSERT(cat_start_publisher_.has_value());
    cat_stopped_publisher_ = cat_stopped_.acquire_publisher();
    FIBER_ASSERT(cat_stopped_publisher_.has_value());
}

AccessServerRuntime::~AccessServerRuntime() {
    FIBER_ASSERT(state_ == AccessServerRuntimeState::Created || state_ == AccessServerRuntimeState::Stopped);
    FIBER_ASSERT(nacos_start_tasks_.empty());
    FIBER_ASSERT(cat_start_tasks_.empty());
}

async::DetachedTask AccessServerRuntime::start_cat() noexcept {
    FIBER_ASSERT(cat_loop_->in_loop());
    const auto started = cat_client_->start();
    if (!started) {
        cat_start_publisher_->publish(CatStartStatus{
                .error = make_io_error(AccessServerRuntimeErrorCode::StartCatClient, started.error()),
        });
    } else {
        cat_start_publisher_->publish(CatStartStatus{.success = true});
    }
    cat_start_tasks_.done();
    co_return;
}

async::DetachedTask AccessServerRuntime::start_nacos() noexcept {
    FIBER_ASSERT(nacos_loop_->in_loop());
    auto client_started = nacos_client_->start();
    if (!client_started) {
        nacos_start_publisher_->publish(NacosStartStatus{
                .error = make_io_error(AccessServerRuntimeErrorCode::StartNacosClient, client_started.error()),
        });
        nacos_start_tasks_.done();
        co_return;
    }
    auto config_started = config_service_->start();
    if (!config_started) {
        nacos_start_publisher_->publish(NacosStartStatus{
                .error = make_io_error(AccessServerRuntimeErrorCode::StartConfigService, config_started.error()),
        });
        nacos_start_tasks_.done();
        co_return;
    }
    auto naming_started = naming_service_->start();
    if (!naming_started) {
        nacos_start_publisher_->publish(NacosStartStatus{
                .error = make_io_error(AccessServerRuntimeErrorCode::StartNamingService, naming_started.error()),
        });
        nacos_start_tasks_.done();
        co_return;
    }
    auto gray_started = gray_watcher_.start();
    if (!gray_started) {
        nacos_start_publisher_->publish(NacosStartStatus{
                .error = make_io_error(AccessServerRuntimeErrorCode::StartGrayWatcher, gray_started.error().io_error,
                                       std::move(gray_started.error().message)),
        });
        nacos_start_tasks_.done();
        co_return;
    }
    auto watcher_started = config_watcher_.start();
    if (!watcher_started) {
        nacos_start_publisher_->publish(NacosStartStatus{
                .error = make_io_error(AccessServerRuntimeErrorCode::StartAccessWatcher,
                                       watcher_started.error().io_error, std::move(watcher_started.error().message)),
        });
        nacos_start_tasks_.done();
        co_return;
    }
    nacos_start_publisher_->publish(NacosStartStatus{.success = true});
    nacos_start_tasks_.done();
}

async::DetachedTask AccessServerRuntime::shutdown_nacos() noexcept {
    FIBER_ASSERT(nacos_loop_->in_loop());
    co_await nacos_start_tasks_.join();
    co_await config_watcher_.shutdown();
    co_await gray_watcher_.shutdown();
    route_store_.clear();
    co_await service_discovery_.shutdown();
    co_await naming_service_->shutdown();
    co_await config_service_->shutdown();
    co_await nacos_client_->shutdown();
    nacos_stopped_publisher_->publish(true);
}

async::DetachedTask AccessServerRuntime::shutdown_cat() noexcept {
    FIBER_ASSERT(cat_loop_->in_loop());
    co_await cat_start_tasks_.join();
    co_await cat_client_->shutdown();
    cat_stopped_publisher_->publish(true);
}

async::Task<void> AccessServerRuntime::stop_nacos() noexcept {
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

async::Task<void> AccessServerRuntime::stop_cat() noexcept {
    FIBER_ASSERT(accept_loop_->in_loop());
    if (!cat_client_) {
        co_return;
    }
    auto stopped = cat_stopped_.subscribe();
    auto snapshot = stopped.current();
    if (!cat_shutdown_spawned_) {
        cat_shutdown_spawned_ = true;
        async::spawn(*cat_loop_, [this]() { return shutdown_cat(); });
    }
    while (!snapshot.value || !*snapshot.value) {
        snapshot = co_await stopped.next(snapshot.version);
    }
}

async::Task<void> AccessServerRuntime::fail_start() noexcept {
    FIBER_ASSERT(accept_loop_->in_loop());
    state_ = AccessServerRuntimeState::Stopping;
    co_await server_.shutdown_and_wait();
    co_await stop_cat();
    co_await stop_nacos();
    state_ = AccessServerRuntimeState::Stopped;
}

async::Task<std::expected<void, AccessServerRuntimeError>> AccessServerRuntime::start() noexcept {
    FIBER_ASSERT(accept_loop_->in_loop());
    FIBER_ASSERT(state_ == AccessServerRuntimeState::Created);
    state_ = AccessServerRuntimeState::Starting;

    auto initialized = server_.initialize();
    if (!initialized) {
        AccessServerRuntimeError error =
                make_io_error(AccessServerRuntimeErrorCode::InitializeWorkers, initialized.error());
        co_await fail_start();
        co_return std::unexpected(std::move(error));
    }

    if (cat_client_) {
        auto cat_status = cat_start_status_.subscribe();
        auto cat_snapshot = cat_status.current();
        cat_start_tasks_.add();
        async::spawn(*cat_loop_, [this]() { return start_cat(); });
        while (!cat_snapshot.value) {
            cat_snapshot = co_await cat_status.next(cat_snapshot.version);
        }
        if (!cat_snapshot.value->success) {
            AccessServerRuntimeError error = cat_snapshot.value->error;
            co_await fail_start();
            co_return std::unexpected(std::move(error));
        }
    }

    auto nacos_status = nacos_start_status_.subscribe();
    auto nacos_snapshot = nacos_status.current();
    nacos_start_tasks_.add();
    async::spawn(*nacos_loop_, [this]() { return start_nacos(); });
    while (!nacos_snapshot.value) {
        nacos_snapshot = co_await nacos_status.next(nacos_snapshot.version);
    }
    if (!nacos_snapshot.value->success) {
        AccessServerRuntimeError error = nacos_snapshot.value->error;
        co_await fail_start();
        co_return std::unexpected(std::move(error));
    }

    auto ready = config_watcher_.subscribe_ready();
    auto ready_snapshot = ready.current();
    if ((!ready_snapshot.value || !*ready_snapshot.value) &&
        initial_config_timeout_ > std::chrono::milliseconds::zero()) {
        auto result =
                co_await async::when_any([&ready, version = ready_snapshot.version]() { return ready.next(version); },
                                         [timeout = initial_config_timeout_]() { return async::sleep(timeout); });
        if (result.is<1>()) {
            std::move(result).get<1>();
            co_await fail_start();
            co_return std::unexpected(make_io_error(AccessServerRuntimeErrorCode::InitialConfigTimeout,
                                                    common::IoErr::TimedOut,
                                                    "initial access project-list synchronization timed out"));
        }
        ready_snapshot = std::move(result).get<0>();
    } else if (!ready_snapshot.value || !*ready_snapshot.value) {
        while (!ready_snapshot.value || !*ready_snapshot.value) {
            ready_snapshot = co_await ready.next(ready_snapshot.version);
        }
    }
    if (!ready_snapshot.value || !*ready_snapshot.value) {
        co_await fail_start();
        co_return std::unexpected(make_io_error(AccessServerRuntimeErrorCode::InitialConfigUnavailable,
                                                common::IoErr::Canceled,
                                                "access project-list subscription closed before synchronization"));
    }

    auto bound = server_.bind(listen_address_, listen_options_);
    if (!bound) {
        const common::IoErr error = bound.error();
        co_await fail_start();
        co_return std::unexpected(make_io_error(AccessServerRuntimeErrorCode::Bind, error));
    }
    auto metrics_bound = server_.bind_metrics(metrics_listen_address_, listen_options_);
    if (!metrics_bound) {
        const common::IoErr error = metrics_bound.error();
        co_await fail_start();
        co_return std::unexpected(make_io_error(AccessServerRuntimeErrorCode::BindMetrics, error));
    }
    state_ = AccessServerRuntimeState::Running;
    async::spawn([this]() { return server_.serve(); });
    async::spawn([this]() { return server_.serve_metrics(); });
    co_return std::expected<void, AccessServerRuntimeError>{};
}

async::Task<void> AccessServerRuntime::shutdown() noexcept {
    FIBER_ASSERT(accept_loop_->in_loop());
    if (state_ == AccessServerRuntimeState::Stopped) {
        co_return;
    }
    state_ = AccessServerRuntimeState::Stopping;
    co_await server_.shutdown_and_wait();
    co_await stop_cat();
    co_await stop_nacos();
    state_ = AccessServerRuntimeState::Stopped;
}

} // namespace fiber::access_server
