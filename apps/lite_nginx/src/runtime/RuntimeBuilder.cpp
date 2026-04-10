#include "RuntimeBuilder.h"

#include <utility>
#include <string_view>
#include <unordered_map>

namespace fiber::lite_nginx::runtime {
namespace {

RuntimeError make_error(const config::SourceLocation &location, std::string message) {
    return RuntimeError{
        .message = std::move(message),
        .location = location,
    };
}

std::string listener_key(const config::ListenAddress &listen) {
    std::string key;
    if (listen.has_host) {
        key = listen.host;
    } else {
        key = "*";
    }
    key.push_back(':');
    key.append(std::to_string(listen.port));
    return key;
}

} // namespace

std::expected<RuntimeConfig, RuntimeError> RuntimeBuilder::build(const config::MainConfig &config) {
    RuntimeConfig runtime;
    runtime.worker_processes = config.worker_processes;
    runtime.servers.reserve(config.http.servers.size());
    runtime.listeners.reserve(config.http.listens.size());

    std::unordered_map<std::string, config::SourceLocation> seen_server_names;
    seen_server_names.reserve(config.http.servers.size() * 2);

    for (const auto &server : config.http.servers) {
        ServerRuntime runtime_server;
        runtime_server.location = server.location;
        runtime_server.server_names = server.server_names;
        runtime_server.certificate = server.certificate;
        runtime_server.certificate_key = server.certificate_key;

        for (const auto &name : server.server_names) {
            auto [it, inserted] = seen_server_names.emplace(name, server.location);
            if (!inserted) {
                return std::unexpected(make_error(
                    server.location,
                    "duplicate server_name is not supported in lite-nginx runtime: " + name));
            }
        }

        runtime.servers.push_back(std::move(runtime_server));
    }

    std::unordered_map<std::string, config::SourceLocation> seen_listeners;
    seen_listeners.reserve(config.http.listens.size());
    for (const auto &listen : config.http.listens) {
        auto key = listener_key(listen);
        auto [it, inserted] = seen_listeners.emplace(key, listen.location);
        if (!inserted) {
            return std::unexpected(make_error(
                listen.location,
                "duplicate listen is not supported in lite-nginx runtime: " + key));
        }

        ListenerRuntime runtime_listener;
        runtime_listener.location = listen.location;
        runtime_listener.host = listen.host;
        runtime_listener.port = listen.port;
        runtime_listener.has_host = listen.has_host;
        runtime_listener.tls = listen.tls;

        if (listen.tls) {
            const auto &default_server = config.http.servers.front();
            runtime_listener.default_certificate = default_server.certificate;
            runtime_listener.default_certificate_key = default_server.certificate_key;

            std::size_t identity_count = 0;
            for (const auto &server : config.http.servers) {
                identity_count += server.server_names.size();
            }
            runtime_listener.tls_identities.reserve(identity_count);
            for (const auto &server : config.http.servers) {
                for (const auto &name : server.server_names) {
                    runtime_listener.tls_identities.push_back({
                        .server_name = name,
                        .certificate = server.certificate,
                        .certificate_key = server.certificate_key,
                    });
                }
            }
        }

        runtime.listeners.push_back(std::move(runtime_listener));
    }

    return runtime;
}

} // namespace fiber::lite_nginx::runtime
