#include "AiServerConfig.h"
#include "AiServerRuntime.h"

#include <csignal>
#include <cstdint>
#include <expected>
#include <iostream>
#include <string_view>
#include <utility>

#include <cerrno>
#include <pthread.h>
#include <sys/socket.h>

#include "async/Signal.h"
#include "async/Spawn.h"
#include "async/WhenAny.h"
#include "common/IoError.h"
#include "event/EventLoop.h"
#include "event/SignalService.h"
#include "net/SocketAddress.h"

namespace {

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
        case Code::AllocateRuntime:
            return "allocate ai-server runtime";
        case Code::Bind:
            return "bind HTTP listener";
        case Code::StartNacosClient:
            return "start Nacos client";
        case Code::StartConfigService:
            return "start Nacos config service";
        case Code::StartConfigManager:
            return "subscribe LLM configuration";
    }
    return "start ai-server";
}

void print_runtime_error(const fiber::ai_server::AiServerRuntimeError &error) {
    std::cerr << runtime_stage_name(error.code) << " failed";
    if (error.io_error != fiber::common::IoErr::None) {
        std::cerr << ": " << fiber::common::io_err_name(error.io_error);
    } else if (!error.message.empty()) {
        std::cerr << ": " << error.message;
    }
    std::cerr << '\n';
}

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

    fiber::event::EventLoop loop;
    fiber::net::ListenOptions listen_options{};
    auto runtime_result = fiber::ai_server::AiServerRuntime::create(loop, config, listen_options);
    if (!runtime_result) {
        print_runtime_error(runtime_result.error());
        return 1;
    }
    std::unique_ptr<fiber::ai_server::AiServerRuntime> runtime = std::move(*runtime_result);

    auto bound_port = resolve_port(runtime->fd());
    if (!bound_port) {
        std::cerr << "resolve bound port failed: " << fiber::common::io_err_name(bound_port.error()) << '\n';
        return 1;
    }

    const fiber::net::SocketAddress bound_address(config.listen_address().ip(), *bound_port);
    fiber::event::SignalService signal_service(loop);
    int exit_code = 0;
    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        if (!signal_service.attach(shutdown_signals)) {
            std::cerr << "failed to attach shutdown signal service\n";
            exit_code = 1;
            loop.stop();
            co_return;
        }

        auto started = co_await runtime->start();
        if (!started) {
            print_runtime_error(started.error());
            exit_code = 1;
            signal_service.detach();
            loop.stop();
            co_return;
        }
        std::cout << "ai-server listening on http://" << bound_address.to_string()
                  << ", nacos servers=" << config.nacos_config().server_ips().size() << std::endl;

        auto signal = co_await fiber::async::when_any([]() { return fiber::async::wait_signal(SIGINT); },
                                                      []() { return fiber::async::wait_signal(SIGTERM); });
        if (signal.is<0>()) {
            (void) std::move(signal).get<0>();
        } else {
            (void) std::move(signal).get<1>();
        }

        co_await runtime->shutdown();
        signal_service.detach();
        loop.stop();
    });
    loop.run();
    return exit_code;
}
