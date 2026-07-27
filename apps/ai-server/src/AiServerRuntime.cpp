#include "AiServerRuntime.h"

#include <thread>

#include <cerrno>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <new>
#include <sys/socket.h>
#include <utility>

#ifdef __linux__
#include <sched.h>
#endif

#include <async/Sleep.h>
#include <async/Spawn.h>
#include <async/TaskSelect.h>
#include <async/WhenAny.h>
#include <common/Assert.h>
#include <log/Log.h>

namespace fiber::ai_server {
namespace {

DEFINE_LOGGER(LOG_LIFECYCLE, "ai_server.lifecycle");

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

AiServerRuntimeError naming_error(nacos::NamingServiceError error) {
    return AiServerRuntimeError{
            .code = AiServerRuntimeErrorCode::StartRateLimitCluster,
            .io_error = error.io_error,
            .naming_error = error.code,
            .message = std::move(error.message),
    };
}

std::optional<net::IpAddress> detect_advertise_ipv4() noexcept {
    ifaddrs *interfaces = nullptr;
    if (::getifaddrs(&interfaces) != 0) {
        return std::nullopt;
    }
    std::optional<net::IpAddress> result;
    for (const ifaddrs *item = interfaces; item; item = item->ifa_next) {
        if (!item->ifa_addr || item->ifa_addr->sa_family != AF_INET || (item->ifa_flags & IFF_UP) == 0) {
            continue;
        }
        net::SocketAddress address;
        if (!net::SocketAddress::from_sockaddr(item->ifa_addr, sizeof(sockaddr_in), address)) {
            continue;
        }
        if (!address.ip().is_unspecified() && !address.ip().is_loopback() && !address.ip().is_multicast()) {
            result = address.ip();
            break;
        }
    }
    ::freeifaddrs(interfaces);
    return result;
}

common::IoResult<std::uint16_t> bound_port(int fd) noexcept {
    sockaddr_storage storage{};
    socklen_t length = sizeof(storage);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&storage), &length) != 0) {
        return std::unexpected(common::io_err_from_errno(errno));
    }
    net::SocketAddress address;
    if (!net::SocketAddress::from_sockaddr(reinterpret_cast<sockaddr *>(&storage), length, address)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return address.port();
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
                        log::AppenderId audit_appender_id, const net::ListenOptions &listen_options) {
    auto client = nacos::NacosClient::create(nacos_loop, config.nacos_config());
    if (!client) {
        return std::unexpected(create_error(AiServerRuntimeErrorCode::CreateNacosClient, client.error()));
    }
    auto service = nacos::ConfigService::create(**client);
    if (!service) {
        return std::unexpected(create_error(AiServerRuntimeErrorCode::CreateConfigService, service.error()));
    }
    auto naming = nacos::NamingService::create(**client);
    if (!naming) {
        return std::unexpected(create_error(AiServerRuntimeErrorCode::CreateNamingService, naming.error()));
    }
    std::unique_ptr<cat::CatClient> cat_client;
    if (config.cat_config()) {
        auto created = cat::CatClient::create(cat_loop, *config.cat_config());
        if (!created) {
            return std::unexpected(AiServerRuntimeError{
                    .code = AiServerRuntimeErrorCode::CreateCatClient,
                    .io_error = common::IoErr::Invalid,
                    .message = "failed to create CAT client",
            });
        }
        cat_client = std::move(*created);
    }
    auto runtime = std::unique_ptr<AiServerRuntime>(new (std::nothrow) AiServerRuntime(
            accept_loop, nacos_loop, cat_loop, http_workers, config.listen_address(), listen_options,
            config.initial_config_timeout(), config.advertise_address(), std::string(config.service_name()),
            std::string(config.service_group()), std::move(cat_client), config.audit_log_options().max_record_bytes,
            audit_appender_id, std::move(*client), std::move(*service), std::move(*naming)));
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
                                 std::optional<net::IpAddress> advertise_address, std::string service_name,
                                 std::string service_group, std::unique_ptr<cat::CatClient> cat_client,
                                 std::size_t audit_max_record_bytes, log::AppenderId audit_appender_id,
                                 std::unique_ptr<nacos::NacosClient> nacos_client,
                                 std::unique_ptr<nacos::ConfigService> config_service,
                                 std::unique_ptr<nacos::NamingService> naming_service) noexcept :
    accept_loop_(&accept_loop), nacos_loop_(&nacos_loop), cat_loop_(&cat_loop),
    listen_address_(std::move(listen_address)), listen_options_(std::move(listen_options)),
    initial_config_timeout_(initial_config_timeout), advertise_address_(std::move(advertise_address)),
    cat_client_(std::move(cat_client)), nacos_client_(std::move(nacos_client)),
    config_service_(std::move(config_service)), naming_service_(std::move(naming_service)),
    config_manager_(nacos_loop, *config_service_, *naming_service_),
    server_(accept_loop, http_workers, cat_client_.get(), audit_max_record_bytes, audit_appender_id),
    rate_limit_membership_(nacos_loop, *naming_service_, server_.rate_limit_ring(), std::move(service_name),
                           std::move(service_group)) {
    FIBER_ASSERT(nacos_client_ != nullptr);
    FIBER_ASSERT(config_service_ != nullptr);
    FIBER_ASSERT(naming_service_ != nullptr);
    FIBER_ASSERT(accept_loop_ != nacos_loop_);
    FIBER_ASSERT(accept_loop_ != cat_loop_);
    FIBER_ASSERT(nacos_loop_ != cat_loop_);
    for (std::size_t i = 0; i < http_workers.size(); ++i) {
        FIBER_ASSERT(&http_workers.at(i) != nacos_loop_);
        FIBER_ASSERT(&http_workers.at(i) != cat_loop_);
    }
    nacos_start_publisher_ = nacos_start_status_.acquire_publisher();
    FIBER_ASSERT(nacos_start_publisher_.has_value());
    cluster_start_publisher_ = cluster_start_status_.acquire_publisher();
    FIBER_ASSERT(cluster_start_publisher_.has_value());
    cat_start_publisher_ = cat_start_status_.acquire_publisher();
    FIBER_ASSERT(cat_start_publisher_.has_value());
    nacos_stopped_publisher_ = nacos_stopped_.acquire_publisher();
    FIBER_ASSERT(nacos_stopped_publisher_.has_value());
    cat_stopped_publisher_ = cat_stopped_.acquire_publisher();
    FIBER_ASSERT(cat_stopped_publisher_.has_value());
}

AiServerRuntime::~AiServerRuntime() {
    FIBER_ASSERT(state_ == AiServerRuntimeState::Created || state_ == AiServerRuntimeState::Stopped);
    FIBER_ASSERT(nacos_start_tasks_.empty());
    FIBER_ASSERT(cluster_start_tasks_.empty());
    FIBER_ASSERT(cat_start_tasks_.empty());
}

async::DetachedTask AiServerRuntime::start_cat() noexcept {
    FIBER_ASSERT(cat_loop_->in_loop());
    const auto started = cat_client_->start();
    if (!started) {
        cat_start_publisher_->publish(CatStartStatus{
                .error = io_error(AiServerRuntimeErrorCode::StartCatClient, started.error()),
        });
    } else {
        LOG(LOG_LIFECYCLE, INFO) << "CAT client started";
        cat_start_publisher_->publish(CatStartStatus{.success = true});
    }
    cat_start_tasks_.done();
    co_return;
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
    LOG(LOG_LIFECYCLE, DEBUG) << "Nacos client started";
    auto service_started = config_service_->start();
    if (!service_started) {
        co_await nacos_client_->shutdown();
        nacos_start_publisher_->publish(NacosStartStatus{
                .error = io_error(AiServerRuntimeErrorCode::StartConfigService, service_started.error()),
        });
        nacos_start_tasks_.done();
        co_return;
    }
    LOG(LOG_LIFECYCLE, DEBUG) << "Nacos config service started";
    auto naming_started = naming_service_->start();
    if (!naming_started) {
        co_await config_service_->shutdown();
        co_await nacos_client_->shutdown();
        nacos_start_publisher_->publish(NacosStartStatus{
                .error = io_error(AiServerRuntimeErrorCode::StartNamingService, naming_started.error()),
        });
        nacos_start_tasks_.done();
        co_return;
    }
    LOG(LOG_LIFECYCLE, DEBUG) << "Nacos naming service started";
    auto manager_started = config_manager_.start();
    if (!manager_started) {
        co_await naming_service_->shutdown();
        co_await config_service_->shutdown();
        co_await nacos_client_->shutdown();
        nacos_start_publisher_->publish(NacosStartStatus{
                .error = config_error(std::move(manager_started.error())),
        });
        nacos_start_tasks_.done();
        co_return;
    }
    LOG(LOG_LIFECYCLE, INFO) << "Nacos-backed LLM configuration runtime started";
    nacos_start_publisher_->publish(NacosStartStatus{.success = true});
    nacos_start_tasks_.done();
}

async::DetachedTask AiServerRuntime::start_rate_limit_cluster(std::string advertise_ipv4, std::uint16_t port) noexcept {
    FIBER_ASSERT(nacos_loop_->in_loop());
    auto started = rate_limit_membership_.start(std::move(advertise_ipv4), port);
    if (!started) {
        cluster_start_publisher_->publish(ClusterStartStatus{
                .error = naming_error(std::move(started.error())),
        });
    } else {
        cluster_start_publisher_->publish(ClusterStartStatus{.success = true});
    }
    cluster_start_tasks_.done();
    co_return;
}

async::DetachedTask AiServerRuntime::shutdown_nacos() noexcept {
    FIBER_ASSERT(nacos_loop_->in_loop());
    co_await nacos_start_tasks_.join();
    co_await cluster_start_tasks_.join();
    co_await rate_limit_membership_.shutdown();
    co_await config_manager_.shutdown();
    co_await naming_service_->shutdown();
    co_await config_service_->shutdown();
    co_await nacos_client_->shutdown();
    LOG(LOG_LIFECYCLE, INFO) << "Nacos-backed LLM configuration runtime stopped";
    nacos_stopped_publisher_->publish(true);
}

async::DetachedTask AiServerRuntime::shutdown_cat() noexcept {
    FIBER_ASSERT(cat_loop_->in_loop());
    co_await cat_start_tasks_.join();
    co_await cat_client_->shutdown();
    LOG(LOG_LIFECYCLE, INFO) << "CAT client stopped";
    cat_stopped_publisher_->publish(true);
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

async::Task<void> AiServerRuntime::stop_cat() noexcept {
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

async::Task<void> AiServerRuntime::fail_start() noexcept {
    FIBER_ASSERT(accept_loop_->in_loop());
    state_ = AiServerRuntimeState::Stopping;
    co_await server_.shutdown_and_wait();
    co_await stop_cat();
    co_await stop_nacos();
    state_ = AiServerRuntimeState::Stopped;
}

async::Task<std::expected<void, AiServerRuntimeError>> AiServerRuntime::start() noexcept {
    FIBER_ASSERT(accept_loop_->in_loop());
    FIBER_ASSERT(state_ == AiServerRuntimeState::Created);
    state_ = AiServerRuntimeState::Starting;

    if (cat_client_) {
        auto cat_status = cat_start_status_.subscribe();
        auto cat_snapshot = cat_status.current();
        cat_start_tasks_.add();
        async::spawn(*cat_loop_, [this]() { return start_cat(); });
        while (!cat_snapshot.value) {
            cat_snapshot = co_await cat_status.next(cat_snapshot.version);
        }
        if (!cat_snapshot.value->success) {
            AiServerRuntimeError error = cat_snapshot.value->error;
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
    LOG(LOG_LIFECYCLE, INFO) << "initial LLM configuration installed on HTTP workers";

    auto bound = server_.bind(listen_address_, listen_options_);
    if (!bound) {
        const common::IoErr error = bound.error();
        co_await fail_start();
        co_return std::unexpected(io_error(AiServerRuntimeErrorCode::Bind, error));
    }

    auto port = bound_port(server_.fd());
    if (!port || *port == 0) {
        const common::IoErr error = port ? common::IoErr::Invalid : port.error();
        co_await fail_start();
        co_return std::unexpected(io_error(AiServerRuntimeErrorCode::Bind, error));
    }
    net::IpAddress advertise = advertise_address_.value_or(net::IpAddress::loopback_v4());
    if (!advertise_address_) {
        const auto detected = detect_advertise_ipv4();
        if (detected) {
            advertise = *detected;
        }
        LOG(LOG_LIFECYCLE, INFO) << "auto-selected ai-server advertise address=" << log::quoted(advertise.to_string());
    }

    auto cluster_status = cluster_start_status_.subscribe();
    auto cluster_snapshot = cluster_status.current();
    cluster_start_tasks_.add();
    async::spawn(*nacos_loop_, [this, address = advertise.to_string(), port = *port]() mutable {
        return start_rate_limit_cluster(std::move(address), port);
    });
    while (!cluster_snapshot.value) {
        cluster_snapshot = co_await cluster_status.next(cluster_snapshot.version);
    }
    if (!cluster_snapshot.value->success) {
        AiServerRuntimeError error = cluster_snapshot.value->error;
        co_await fail_start();
        co_return std::unexpected(std::move(error));
    }
    state_ = AiServerRuntimeState::Running;
    async::spawn([this]() { return server_.serve(); });
    co_return std::expected<void, AiServerRuntimeError>{};
}

async::Task<void> AiServerRuntime::shutdown() noexcept {
    FIBER_ASSERT(accept_loop_->in_loop());
    if (state_ == AiServerRuntimeState::Stopped) {
        co_return;
    }
    LOG(LOG_LIFECYCLE, INFO) << "runtime shutdown started";
    if (state_ == AiServerRuntimeState::Created) {
        server_.close();
        co_await stop_cat();
        co_await stop_nacos();
        state_ = AiServerRuntimeState::Stopped;
        LOG(LOG_LIFECYCLE, INFO) << "runtime shutdown completed";
        co_return;
    }

    state_ = AiServerRuntimeState::Stopping;
    co_await server_.shutdown_and_wait();
    co_await stop_cat();
    co_await stop_nacos();
    state_ = AiServerRuntimeState::Stopped;
    LOG(LOG_LIFECYCLE, INFO) << "runtime shutdown completed";
}

} // namespace fiber::ai_server
