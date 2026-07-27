#include "AiServerConfig.h"
#include "AiServerLogging.h"
#include "AiServerRuntime.h"

#include <csignal>
#include <cstdint>
#include <cstring>
#include <expected>
#include <iostream>
#include <string_view>
#include <utility>

#include <cerrno>
#include <pthread.h>
#include <sys/socket.h>

#include "async/Signal.h"
#include "async/Spawn.h"
#include "async/TaskSelect.h"
#include "async/WhenAny.h"
#include "common/IoError.h"
#include "event/EventLoop.h"
#include "event/EventLoopGroup.h"
#include "event/SignalService.h"
#include "log/Log.h"
#include "net/SocketAddress.h"

namespace {

DEFINE_LOGGER(LOG_LIFECYCLE, "ai_server.lifecycle");

fiber::common::IoResult<std::uint16_t> resolve_port(int fd) noexcept {
    sockaddr_storage bound{};
    socklen_t length = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&bound), &length) != 0) {
        return std::unexpected(fiber::common::io_err_from_errno(errno));
    }

    fiber::net::SocketAddress address;
    if (!fiber::net::SocketAddress::from_sockaddr(reinterpret_cast<sockaddr *>(&bound), length, address)) {
        return std::unexpected(fiber::common::IoErr::NotSupported);
    }
    return address.port();
}

void print_config_error(std::string_view path, const fiber::ai_server::AiServerConfigError &error) {
    std::cerr << "configuration error in " << path;
    if (error.line != 0) {
        std::cerr << ':' << error.line;
    }
    if (!error.key.empty()) {
        std::cerr << " [" << error.key << ']';
    }
    std::cerr << ": " << error.detail << '\n';
}

std::string_view runtime_stage_name(fiber::ai_server::AiServerRuntimeErrorCode code) noexcept {
    using Code = fiber::ai_server::AiServerRuntimeErrorCode;
    switch (code) {
        case Code::CreateNacosClient:
            return "create Nacos client";
        case Code::CreateConfigService:
            return "create Nacos config service";
        case Code::CreateNamingService:
            return "create Nacos naming service";
        case Code::CreateCatClient:
            return "create CAT client";
        case Code::AllocateRuntime:
            return "allocate ai-server runtime";
        case Code::Bind:
            return "bind HTTP listener";
        case Code::StartNacosClient:
            return "start Nacos client";
        case Code::StartConfigService:
            return "start Nacos config service";
        case Code::StartNamingService:
            return "start Nacos naming service";
        case Code::StartCatClient:
            return "start CAT client";
        case Code::StartConfigManager:
            return "subscribe LLM configuration";
        case Code::StartRateLimitCluster:
            return "start token rate limit cluster";
        case Code::InitialConfigUnavailable:
            return "install initial LLM configuration";
        case Code::InitialConfigTimeout:
            return "wait for initial LLM configuration";
    }
    return "start ai-server";
}

void log_runtime_error(const fiber::ai_server::AiServerRuntimeError &error) noexcept {
    fiber::log::LogLine line(LOG_LIFECYCLE.get(), fiber::log::LogLevel::Error, __FILE__, __LINE__, __func__);
    line << "runtime startup failed stage=" << fiber::log::quoted(runtime_stage_name(error.code));
    if (error.io_error != fiber::common::IoErr::None) {
        line << " io_error=" << fiber::common::io_err_name(error.io_error);
    } else if (!error.message.empty()) {
        line << " reason=" << fiber::log::quoted(error.message);
    }
}

void print_log_error(const fiber::log::LogConfigError &error) {
    std::cerr << "failed to initialize logging: " << error.message;
    if (error.system_error != 0) {
        std::cerr << ": " << std::strerror(error.system_error);
    }
    std::cerr << '\n';
}

class LoggingShutdownGuard {
public:
    ~LoggingShutdownGuard() { fiber::log::LoggerManager::global().shutdown(); }
};

} // namespace

int main(int argc, char **argv) {
    if (argc > 2) {
        std::cerr << "usage: ai-server [config.env]\n";
        return 1;
    }

    const std::string_view config_path = argc == 2 ? std::string_view(argv[1]) : std::string_view("ai-server.env");
    auto config_result = fiber::ai_server::AiServerConfig::load_from_file(config_path);
    if (!config_result) {
        print_config_error(config_path, config_result.error());
        return 1;
    }
    fiber::ai_server::AiServerConfig config = std::move(*config_result);

    (void) ::signal(SIGPIPE, SIG_IGN);
    fiber::async::SignalSet shutdown_signals;
    shutdown_signals.add(SIGINT).add(SIGTERM);
    if (::pthread_sigmask(SIG_BLOCK, &shutdown_signals.native(), nullptr) != 0) {
        std::cerr << "failed to block shutdown signals\n";
        return 1;
    }

    auto log_config = fiber::ai_server::make_log_config(config.audit_log_options());
    if (!log_config) {
        print_log_error(log_config.error());
        return 1;
    }
    const fiber::log::AppenderId audit_appender_id = log_config->audit_appender_id;
    auto logging_started = fiber::log::LoggerManager::global().initialize(std::move(log_config->config));
    if (!logging_started) {
        print_log_error(logging_started.error());
        return 1;
    }

    fiber::event::EventLoop accept_loop;
    fiber::event::EventLoopGroup http_workers(fiber::ai_server::default_http_worker_count());
    fiber::event::EventLoopGroup nacos_group(1);
    fiber::event::EventLoopGroup cat_group(1);
    LoggingShutdownGuard logging_guard;
    fiber::net::ListenOptions listen_options{};
    LOG(LOG_LIFECYCLE, INFO) << "configuration loaded path=" << fiber::log::quoted(config_path)
                             << " listen=" << fiber::log::quoted(config.listen_address().to_string())
                             << " http_workers=" << http_workers.size()
                             << " nacos_servers=" << config.nacos_config().server_ips().size()
                             << " audit_path=" << fiber::log::quoted(config.audit_log_options().path);

    auto runtime_result = fiber::ai_server::AiServerRuntime::create(
            accept_loop, nacos_group.at(0), cat_group.at(0), http_workers, config, audit_appender_id, listen_options);
    if (!runtime_result) {
        log_runtime_error(runtime_result.error());
        return 1;
    }
    std::unique_ptr<fiber::ai_server::AiServerRuntime> runtime = std::move(*runtime_result);

    http_workers.start(shutdown_signals);
    nacos_group.start(shutdown_signals);
    cat_group.start(shutdown_signals);

    fiber::event::SignalService signal_service(accept_loop);
    int exit_code = 0;
    fiber::async::spawn(accept_loop, [&]() -> fiber::async::DetachedTask {
        if (!signal_service.attach(shutdown_signals)) {
            LOG(LOG_LIFECYCLE, ERROR) << "failed to attach shutdown signal service";
            exit_code = 1;
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
            LOG(LOG_LIFECYCLE, INFO) << "shutdown signal received during startup";
            co_await runtime->shutdown();
            signal_service.detach();
            accept_loop.stop();
            co_return;
        }

        auto started = std::move(startup).get<0>();
        if (!started) {
            log_runtime_error(started.error());
            exit_code = 1;
            signal_service.detach();
            accept_loop.stop();
            co_return;
        }

        auto bound_port = resolve_port(runtime->fd());
        if (!bound_port) {
            LOG(LOG_LIFECYCLE, ERROR) << "resolve bound port failed io_error="
                                      << fiber::common::io_err_name(bound_port.error());
            exit_code = 1;
            co_await runtime->shutdown();
            signal_service.detach();
            accept_loop.stop();
            co_return;
        }
        const fiber::net::SocketAddress bound_address(config.listen_address().ip(), *bound_port);
        LOG(LOG_LIFECYCLE, INFO) << "server listening scheme=http address="
                                 << fiber::log::quoted(bound_address.to_string())
                                 << " http_workers=" << http_workers.size()
                                 << " nacos_servers=" << config.nacos_config().server_ips().size();
        std::cout << "ai-server listening on http://" << bound_address.to_string()
                  << ", http workers=" << http_workers.size()
                  << ", nacos servers=" << config.nacos_config().server_ips().size() << std::endl;

        auto signal = co_await fiber::async::when_any([]() { return fiber::async::wait_signal(SIGINT); },
                                                      []() { return fiber::async::wait_signal(SIGTERM); });
        if (signal.is<0>()) {
            (void) std::move(signal).get<0>();
        } else {
            (void) std::move(signal).get<1>();
        }

        LOG(LOG_LIFECYCLE, INFO) << "shutdown signal received";
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
    LOG(LOG_LIFECYCLE, INFO) << "ai-server stopped";
    return exit_code;
}
