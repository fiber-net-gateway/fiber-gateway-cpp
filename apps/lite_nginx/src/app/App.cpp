#include "App.h"

#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

#include <fiber/dns/DnsResolverConfig.h>
#include <fiber/event/EventLoop.h>
#include <fiber/log/Log.h>
#include "config/Config.h"
#include "config/ConfigLoader.h"
#include "logging/LoggingBuilder.h"
#include "runtime/RuntimeBuilder.h"
#include "runtime/ServerLauncher.h"

namespace fiber::lite_nginx::app {
namespace {

DEFINE_LOGGER(LOG_LIFECYCLE, "lite_nginx.lifecycle");

struct Options {
    std::string config_path;
    bool check_config = false;
};

void print_usage(const char *argv0) { std::cerr << "usage: " << argv0 << " [--check-config] [--config <path>]\n"; }

std::string format_config_error(const config::ConfigError &error) {
    std::string formatted;
    if (!error.location.source_name.empty()) {
        formatted.append(error.location.source_name);
    } else {
        formatted.append("<config>");
    }
    formatted.push_back(':');
    formatted.append(std::to_string(error.location.line));
    formatted.push_back(':');
    formatted.append(std::to_string(error.location.column));
    formatted.append(": ");
    formatted.append(error.message);
    return formatted;
}

std::string format_runtime_error(const runtime::RuntimeError &error) {
    std::string formatted;
    if (!error.location.source_name.empty()) {
        formatted.append(error.location.source_name);
        formatted.push_back(':');
        formatted.append(std::to_string(error.location.line));
        formatted.push_back(':');
        formatted.append(std::to_string(error.location.column));
        formatted.append(": ");
    }
    formatted.append(error.message);
    return formatted;
}

bool parse_options(int argc, char **argv, Options &options) {
    options.config_path = std::string(FIBER_LITE_NGINX_SOURCE_DIR) + "/conf/lite_nginx.conf";

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--check-config") {
            options.check_config = true;
            continue;
        }
        if (arg == "--config") {
            if (i + 1 >= argc) {
                return false;
            }
            options.config_path = argv[++i];
            continue;
        }
        if (arg == "--help" || arg == "-h") {
            return false;
        }
        return false;
    }
    return true;
}

void print_summary(const config::MainConfig &config, std::string_view path) {
    std::size_t tls_listeners = 0;
    std::size_t http3_listeners = 0;
    for (const auto &listen: config.http.listens) {
        if (listen.tls) {
            ++tls_listeners;
        }
        if (listen.http3) {
            ++http3_listeners;
        }
    }

    std::cout << "config ok: " << path << '\n';
    std::cout << "worker_processes=" << config.worker_processes << '\n';
    std::cout << "listeners=" << config.http.listens.size() << '\n';
    std::cout << "tls_listeners=" << tls_listeners << '\n';
    std::cout << "http3_listeners=" << http3_listeners << '\n';
    std::cout << "upstreams=" << config.http.upstreams.size() << '\n';
    std::cout << "servers=" << config.http.servers.size() << '\n';
}

std::string format_log_init_error(const fiber::log::LogConfigError &error) {
    std::string formatted = error.message;
    if (error.system_error != 0) {
        formatted.append(": ");
        formatted.append(std::strerror(error.system_error));
    }
    return formatted;
}

std::string format_resolver_config_error(const fiber::dns::ResolverConfigError &error) {
    std::string formatted(fiber::dns::resolver_config_error_name(error.code));
    if (error.line != 0) {
        formatted.append(" at /etc/resolv.conf:");
        formatted.append(std::to_string(error.line));
    }
    if (error.system_error != 0) {
        formatted.append(": ");
        formatted.append(std::strerror(error.system_error));
    }
    return formatted;
}

class LoggingShutdownGuard {
public:
    ~LoggingShutdownGuard() { fiber::log::LoggerManager::global().shutdown(); }
};

} // namespace

int LiteNginxApp::run(int argc, char **argv) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        print_usage(argv[0]);
        return 1;
    }

    auto config_result = config::ConfigLoader::load_from_file(options.config_path);
    if (!config_result) {
        std::cerr << format_config_error(config_result.error()) << '\n';
        return 1;
    }

    auto log_config_result = logging::LoggingBuilder::build(*config_result);
    if (!log_config_result) {
        std::cerr << format_runtime_error(log_config_result.error()) << '\n';
        return 1;
    }

    auto runtime_result = runtime::RuntimeBuilder::build(*config_result);
    if (!runtime_result) {
        std::cerr << format_runtime_error(runtime_result.error()) << '\n';
        return 1;
    }

    if (options.check_config) {
        print_summary(*config_result, options.config_path);
        return 0;
    }

    auto resolver_config_result = fiber::dns::load_system_resolver_config();
    if (!resolver_config_result) {
        std::cerr << "failed to load system resolver configuration: "
                  << format_resolver_config_error(resolver_config_result.error()) << '\n';
        return 1;
    }

    auto log_init_result = fiber::log::LoggerManager::global().initialize(std::move(*log_config_result));
    if (!log_init_result) {
        std::cerr << "failed to initialize logging: " << format_log_init_error(log_init_result.error()) << '\n';
        return 1;
    }
    LoggingShutdownGuard logging_guard;
    LOG(LOG_LIFECYCLE, INFO) << "configuration loaded path=" << fiber::log::quoted(options.config_path)
                             << " workers=" << config_result->worker_processes
                             << " listeners=" << config_result->http.listens.size()
                             << " upstreams=" << config_result->http.upstreams.size()
                             << " servers=" << config_result->http.servers.size();
    if (resolver_config_result->unsupported != fiber::dns::ResolverUnsupportedFeature::None) {
        LOG(LOG_LIFECYCLE, WARN) << "ignoring unsupported /etc/resolv.conf settings first_line="
                                 << resolver_config_result->first_unsupported_line;
    }

    fiber::event::EventLoop loop;
    runtime::ServerLauncher launcher(loop);
    auto start_result = launcher.start(*runtime_result, *resolver_config_result);
    if (!start_result) {
        LOG(LOG_LIFECYCLE, ERROR) << format_runtime_error(start_result.error());
        return 1;
    }

    for (const auto &listener: launcher.bound_listeners()) {
        LOG(LOG_LIFECYCLE, INFO) << "listening scheme=" << (listener.tls ? "https" : "http")
                                 << " address=" << fiber::log::quoted(listener.address.to_string());
        if (listener.http3) {
            LOG(LOG_LIFECYCLE, INFO) << "listening scheme=h3 address="
                                     << fiber::log::quoted(listener.address.to_string());
        }
    }

    LOG(LOG_LIFECYCLE, INFO) << "reverse proxy runtime started";
    loop.run();
    launcher.close();
    LOG(LOG_LIFECYCLE, INFO) << "reverse proxy runtime stopped";
    return 0;
}

} // namespace fiber::lite_nginx::app
