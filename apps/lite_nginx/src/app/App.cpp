#include "App.h"

#include <iostream>
#include <string>
#include <string_view>

#include "config/Config.h"
#include "config/ConfigLoader.h"
#include "event/EventLoop.h"
#include "runtime/RuntimeBuilder.h"
#include "runtime/ServerLauncher.h"

namespace fiber::lite_nginx::app {
namespace {

struct Options {
    std::string config_path;
    bool check_config = false;
};

void print_usage(const char *argv0) {
    std::cerr << "usage: " << argv0 << " [--check-config] [--config <path>]\n";
}

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
    for (const auto &listen : config.http.listens) {
        if (listen.tls) {
            ++tls_listeners;
        }
    }

    std::cout << "config ok: " << path << '\n';
    std::cout << "worker_processes=" << config.worker_processes << '\n';
    std::cout << "listeners=" << config.http.listens.size() << '\n';
    std::cout << "tls_listeners=" << tls_listeners << '\n';
    std::cout << "upstreams=" << config.http.upstreams.size() << '\n';
    std::cout << "servers=" << config.http.servers.size() << '\n';
}

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

    print_summary(*config_result, options.config_path);
    if (options.check_config) {
        return 0;
    }

    auto runtime_result = runtime::RuntimeBuilder::build(*config_result);
    if (!runtime_result) {
        std::cerr << format_runtime_error(runtime_result.error()) << '\n';
        return 1;
    }

    fiber::event::EventLoop loop;
    runtime::ServerLauncher launcher(loop);
    auto start_result = launcher.start(*runtime_result);
    if (!start_result) {
        std::cerr << format_runtime_error(start_result.error()) << '\n';
        return 1;
    }

    for (const auto &listener : launcher.bound_listeners()) {
        std::cout << "listening on " << (listener.tls ? "https://" : "http://")
                  << listener.address.to_string() << '\n';
    }

    std::cout << "every request currently returns: hello lite nginx\n";
    loop.run();
    return 0;
}

} // namespace fiber::lite_nginx::app
