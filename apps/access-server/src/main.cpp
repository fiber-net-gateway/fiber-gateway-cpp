#include "observability/AccessServerLogCategories.h"
#include "runtime/AccessServerConfig.h"
#include "runtime/AccessServerRuntime.h"

#include <csignal>
#include <cstdint>
#include <cstring>
#include <expected>
#include <iostream>
#include <memory>
#include <string_view>
#include <utility>

#include <cerrno>
#include <pthread.h>
#include <sys/socket.h>

#include <fiber/async/Signal.h>
#include <fiber/async/Spawn.h>
#include <fiber/async/TaskSelect.h>
#include <fiber/async/WhenAny.h>
#include <fiber/common/IoError.h>
#include <fiber/common/util/CpuConcurrency.h>
#include <fiber/event/EventLoop.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/event/SignalService.h>
#include <fiber/log/Log.h>
#include <fiber/log/LogConfig.h>
#include <fiber/log/LoggerManager.h>
#include <fiber/net/SocketAddress.h>

namespace {

DEFINE_LOGGER(LOG_LIFECYCLE, fiber::access_server::kAccessServerLifecycleLogger);

constexpr std::string_view kUsage = "usage: access-server [config.env]\n";

fiber::common::IoResult<std::uint16_t> bound_port(int fd) noexcept {
    sockaddr_storage storage{};
    socklen_t length = sizeof(storage);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&storage), &length) != 0) {
        return std::unexpected(fiber::common::io_err_from_errno(errno));
    }
    fiber::net::SocketAddress address;
    if (!fiber::net::SocketAddress::from_sockaddr(reinterpret_cast<sockaddr *>(&storage), length, address)) {
        return std::unexpected(fiber::common::IoErr::Invalid);
    }
    return address.port();
}

void print_config_error(std::string_view path, const fiber::access_server::AccessServerConfigError &error) {
    std::cerr << "configuration error in " << path;
    if (error.line != 0) {
        std::cerr << ':' << error.line;
    }
    if (!error.key.empty()) {
        std::cerr << " [" << error.key << ']';
    }
    std::cerr << ": " << error.detail << '\n';
}

void print_runtime_error(const fiber::access_server::AccessServerRuntimeError &error) {
    std::cerr << "access-server startup failed at "
              << fiber::access_server::access_server_runtime_stage_name(error.code);
    if (error.io_error != fiber::common::IoErr::None) {
        std::cerr << ": " << fiber::common::io_err_name(error.io_error);
    }
    if (!error.message.empty()) {
        std::cerr << ": " << error.message;
    }
    std::cerr << '\n';
}

fiber::log::LogConfigResult<fiber::log::LogConfig> make_log_config() {
    fiber::log::LogConfigBuilder builder;
    auto console = builder.add_console_appender({
            .name = "access_server_stderr",
            .stream = fiber::log::ConsoleStream::Stderr,
    });
    if (!console) {
        return std::unexpected(std::move(console.error()));
    }
    auto queue = builder.set_async_options({
            .backlog_capacity = fiber::log::kDefaultLogBacklogCapacity,
            .full_policy = fiber::log::LogQueueFullPolicy::DropNewest,
    });
    if (!queue) {
        return std::unexpected(std::move(queue.error()));
    }
    auto root = builder.set_root_logger(
            {
                    .level = fiber::log::LogLevel::Info,
            },
            {*console});
    if (!root) {
        return std::unexpected(std::move(root.error()));
    }
    return builder.finish();
}

class LoggingShutdownGuard {
public:
    ~LoggingShutdownGuard() { fiber::log::LoggerManager::global().shutdown(); }
};

} // namespace

int main(int argc, char **argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << kUsage;
        return 0;
    }
    if (argc > 2) {
        std::cerr << kUsage;
        return 1;
    }

    const std::string_view config_path = argc == 2 ? std::string_view(argv[1]) : std::string_view("access-server.env");
    auto loaded = fiber::access_server::AccessServerConfig::load_from_file(config_path);
    if (!loaded) {
        print_config_error(config_path, loaded.error());
        return 1;
    }
    fiber::access_server::AccessServerConfig config = std::move(*loaded);

    (void) ::signal(SIGPIPE, SIG_IGN);
    fiber::async::SignalSet shutdown_signals;
    shutdown_signals.add(SIGINT).add(SIGTERM);
    if (::pthread_sigmask(SIG_BLOCK, &shutdown_signals.native(), nullptr) != 0) {
        std::cerr << "failed to block shutdown signals\n";
        return 1;
    }

    auto log_config = make_log_config();
    if (!log_config) {
        std::cerr << "failed to build access-server logging configuration: " << log_config.error().message << '\n';
        return 1;
    }
    auto logging_started = fiber::log::LoggerManager::global().initialize(std::move(*log_config));
    if (!logging_started) {
        std::cerr << "failed to initialize access-server logging: " << logging_started.error().message << '\n';
        return 1;
    }
    LoggingShutdownGuard logging_guard;

    const fiber::util::CpuConcurrency cpu = fiber::util::detect_cpu_concurrency();
    fiber::event::EventLoop accept_loop;
    fiber::event::EventLoopGroup http_workers(cpu.effective_count);
    fiber::event::EventLoopGroup nacos_group(1);
    fiber::event::EventLoopGroup cat_group(1);
    auto created = fiber::access_server::AccessServerRuntime::create(accept_loop, nacos_group.at(0), cat_group.at(0),
                                                                     http_workers, config);
    if (!created) {
        print_runtime_error(created.error());
        return 1;
    }
    std::unique_ptr<fiber::access_server::AccessServerRuntime> runtime = std::move(*created);

    http_workers.start(shutdown_signals);
    nacos_group.start(shutdown_signals);
    cat_group.start(shutdown_signals);

    fiber::event::SignalService signal_service(accept_loop);
    int exit_code = 0;
    fiber::async::spawn(accept_loop, [&]() -> fiber::async::DetachedTask {
        if (!signal_service.attach(shutdown_signals)) {
            LOG(LOG_LIFECYCLE, ERROR) << "failed to attach shutdown signal service";
            std::cerr << "failed to attach shutdown signal service\n";
            exit_code = 1;
            co_await runtime->shutdown();
            accept_loop.stop();
            co_return;
        }

        auto startup = co_await fiber::async::when_any([&runtime]() { return runtime->start().select(); },
                                                       []() { return fiber::async::wait_signal(SIGINT); },
                                                       []() { return fiber::async::wait_signal(SIGTERM); });
        if (!startup.is<0>()) {
            if (startup.is<1>()) {
                (void) std::move(startup).get<1>();
            } else {
                (void) std::move(startup).get<2>();
            }
            co_await runtime->shutdown();
            signal_service.detach();
            accept_loop.stop();
            co_return;
        }

        auto started = std::move(startup).get<0>();
        if (!started) {
            print_runtime_error(started.error());
            exit_code = 1;
            signal_service.detach();
            accept_loop.stop();
            co_return;
        }

        auto port = bound_port(runtime->fd());
        if (!port) {
            std::cerr << "failed to resolve bound listener port: " << fiber::common::io_err_name(port.error()) << '\n';
            exit_code = 1;
            co_await runtime->shutdown();
            signal_service.detach();
            accept_loop.stop();
            co_return;
        }
        const fiber::net::SocketAddress address(config.listen_address().ip(), *port);
        auto metrics_port = bound_port(runtime->metrics_fd());
        if (!metrics_port) {
            std::cerr << "failed to resolve metrics listener port: " << fiber::common::io_err_name(metrics_port.error())
                      << '\n';
            exit_code = 1;
            co_await runtime->shutdown();
            signal_service.detach();
            accept_loop.stop();
            co_return;
        }
        const fiber::net::SocketAddress metrics_address(config.metrics_listen_address().ip(), *metrics_port);
        std::cout << "access-server listening on http://" << address.to_string() << ", metrics=http://"
                  << metrics_address.to_string() << ", http workers=" << http_workers.size()
                  << ", nacos servers=" << config.nacos_config().server_ips().size() << std::endl;
        LOG(LOG_LIFECYCLE, INFO) << "server listening address=" << fiber::log::quoted(address.to_string())
                                 << " metrics_address=" << fiber::log::quoted(metrics_address.to_string())
                                 << " http_workers=" << http_workers.size() << " cpu_affinity=" << cpu.affinity_count
                                 << " cpu_quota_workers=" << cpu.quota_count << " cpu_quota_us=" << cpu.quota_us
                                 << " cpu_period_us=" << cpu.period_us
                                 << " cpu_concurrency_source=" << fiber::util::cpu_concurrency_source_name(cpu.source)
                                 << " cgroup_probe_failed=" << cpu.cgroup_probe_failed
                                 << " nacos_servers=" << config.nacos_config().server_ips().size()
                                 << " cat_enabled=" << config.cat_config().has_value();

        auto signal = co_await fiber::async::when_any([]() { return fiber::async::wait_signal(SIGINT); },
                                                      []() { return fiber::async::wait_signal(SIGTERM); });
        if (signal.is<0>()) {
            (void) std::move(signal).get<0>();
        } else {
            (void) std::move(signal).get<1>();
        }
        co_await runtime->shutdown();
        signal_service.detach();
        accept_loop.stop();
    });
    accept_loop.run();

    http_workers.stop();
    nacos_group.stop();
    cat_group.stop();
    http_workers.join();
    nacos_group.join();
    cat_group.join();
    return exit_code;
}
