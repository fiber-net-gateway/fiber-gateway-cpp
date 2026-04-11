#include "RuntimeBuilder.h"

#include <algorithm>
#include <chrono>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "common/route/RoutePathMatcher.h"
#include "http/HttpHeaderHash.h"
#include "net/IpAddress.h"

namespace fiber::lite_nginx::runtime {
namespace {

constexpr std::chrono::milliseconds kDefaultConnectTimeout{10000};
constexpr std::chrono::milliseconds kDefaultReadTimeout{30000};
constexpr std::chrono::milliseconds kDefaultSendTimeout{30000};

using fiber::common::route::RoutePatternError;

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

std::string direct_upstream_key(std::string_view host, std::uint16_t port) {
    std::string key(host);
    key.push_back(':');
    key.append(std::to_string(port));
    return key;
}

std::string compile_location_pattern(const config::LocationConfig &location) {
    if (location.match_kind == config::LocationMatchKind::Exact) {
        return location.pattern;
    }
    if (location.pattern.find(':') != std::string::npos || location.pattern.find('*') != std::string::npos) {
        return location.pattern;
    }
    if (location.pattern == "/") {
        return "/*";
    }

    std::string compiled = location.pattern;
    if (!compiled.empty() && compiled.back() != '/') {
        compiled.push_back('/');
    }
    compiled.push_back('*');
    return compiled;
}

std::chrono::milliseconds resolve_timeout(const std::optional<std::chrono::milliseconds> &override_value,
                                          std::chrono::milliseconds inherited,
                                          std::chrono::milliseconds fallback) {
    if (override_value.has_value()) {
        return *override_value;
    }
    if (inherited.count() > 0) {
        return inherited;
    }
    return fallback;
}

std::expected<UpstreamPeerRuntime, RuntimeError> make_peer_runtime(const config::SourceLocation &location,
                                                                   std::string host,
                                                                   std::uint16_t port) {
    fiber::net::IpAddress ip;
    if (!fiber::net::IpAddress::parse(host, ip)) {
        return std::unexpected(make_error(
            location,
            "upstream host must be an IP literal in lite-nginx runtime: " + host));
    }

    UpstreamPeerRuntime peer;
    peer.host = std::move(host);
    peer.port = port;
    peer.ip = ip;
    peer.address = fiber::net::SocketAddress(ip, port);
    peer.connection_key = fiber::http::Http1ConnectionGroupKey::from_ip(
        ip,
        port,
        fiber::http::Http1ConnectionGroupKey::Scheme::Http);
    return peer;
}

struct LocationRoutePayload {
    std::uint32_t location_index = 0;
};

struct LocationRouteDefiner {
    void add_path_var_definer(LocationRoutePayload &, std::string_view, std::uint32_t) {}

    std::uint32_t on_route_mount(std::uint32_t, std::string_view, LocationRoutePayload &payload) {
        return payload.location_index;
    }
};

} // namespace

std::expected<RuntimeConfig, RuntimeError> RuntimeBuilder::build(const config::MainConfig &config) {
    RuntimeConfig runtime;
    runtime.worker_processes = config.worker_processes;
    runtime.upstreams.reserve(config.http.upstreams.size());
    runtime.servers.reserve(config.http.servers.size());
    runtime.listeners.reserve(config.http.listens.size());

    std::unordered_map<std::string, std::uint32_t> upstream_indices;
    upstream_indices.reserve(config.http.upstreams.size() * 2 + 4);

    for (const auto &upstream : config.http.upstreams) {
        UpstreamRuntime runtime_upstream;
        runtime_upstream.name = upstream.name;
        runtime_upstream.keepalive = upstream.keepalive;
        runtime_upstream.connect_timeout = upstream.connect_timeout.value_or(kDefaultConnectTimeout);
        runtime_upstream.read_timeout = upstream.read_timeout.value_or(kDefaultReadTimeout);
        runtime_upstream.send_timeout = upstream.send_timeout.value_or(kDefaultSendTimeout);
        runtime_upstream.peers.reserve(upstream.servers.size());

        for (const auto &server : upstream.servers) {
            auto peer_result = make_peer_runtime(config::SourceLocation{}, server.host, server.port);
            if (!peer_result) {
                return std::unexpected(peer_result.error());
            }
            runtime_upstream.peers.push_back(std::move(*peer_result));
        }

        const std::uint32_t index = static_cast<std::uint32_t>(runtime.upstreams.size());
        upstream_indices.emplace(runtime_upstream.name, index);
        runtime.upstreams.push_back(std::move(runtime_upstream));
    }

    std::unordered_map<std::string, std::uint32_t> direct_upstream_indices;
    direct_upstream_indices.reserve(config.http.servers.size() * 2 + 4);

    std::unordered_map<std::string, config::SourceLocation> seen_server_names;
    seen_server_names.reserve(config.http.servers.size() * 2);

    for (const auto &server : config.http.servers) {
        ServerRuntime runtime_server;
        runtime_server.location = server.location;
        runtime_server.server_names = server.server_names;
        runtime_server.certificate = server.certificate;
        runtime_server.certificate_key = server.certificate_key;
        runtime_server.locations.reserve(server.locations.size());

        LocationRouteDefiner route_definer;
        fiber::common::route::RoutePathMatcher<std::uint32_t>::Builder<LocationRoutePayload, LocationRouteDefiner>
            matcher_builder(route_definer);

        for (const auto &name : server.server_names) {
            auto [it, inserted] = seen_server_names.emplace(name, server.location);
            if (!inserted) {
                return std::unexpected(make_error(
                    server.location,
                    "duplicate server_name is not supported in lite-nginx runtime: " + name));
            }
        }

        for (const auto &location : server.locations) {
            std::uint32_t upstream_index = 0;
            std::string default_host_header;
            std::chrono::milliseconds inherited_connect = kDefaultConnectTimeout;
            std::chrono::milliseconds inherited_read = kDefaultReadTimeout;
            std::chrono::milliseconds inherited_send = kDefaultSendTimeout;

            if (location.proxy_pass.kind == config::ProxyPassKind::NamedUpstream) {
                auto it = upstream_indices.find(location.proxy_pass.upstream_name);
                if (it == upstream_indices.end()) {
                    return std::unexpected(make_error(
                        location.proxy_pass.location,
                        "proxy_pass references unknown upstream in runtime: " + location.proxy_pass.upstream_name));
                }
                upstream_index = it->second;
                const UpstreamRuntime &upstream = runtime.upstreams[upstream_index];
                inherited_connect = upstream.connect_timeout;
                inherited_read = upstream.read_timeout;
                inherited_send = upstream.send_timeout;
                default_host_header = location.proxy_pass.upstream_name;
            } else {
                const std::string key = direct_upstream_key(location.proxy_pass.host, location.proxy_pass.port);
                auto it = direct_upstream_indices.find(key);
                if (it == direct_upstream_indices.end()) {
                    auto peer_result = make_peer_runtime(
                        location.proxy_pass.location,
                        location.proxy_pass.host,
                        location.proxy_pass.port);
                    if (!peer_result) {
                        return std::unexpected(peer_result.error());
                    }

                    UpstreamRuntime upstream;
                    upstream.peers.push_back(std::move(*peer_result));
                    upstream.connect_timeout = kDefaultConnectTimeout;
                    upstream.read_timeout = kDefaultReadTimeout;
                    upstream.send_timeout = kDefaultSendTimeout;

                    upstream_index = static_cast<std::uint32_t>(runtime.upstreams.size());
                    runtime.upstreams.push_back(std::move(upstream));
                    direct_upstream_indices.emplace(key, upstream_index);
                } else {
                    upstream_index = it->second;
                }
                default_host_header = location.proxy_pass.host;
            }

            LocationRuntime runtime_location;
            runtime_location.location = location.proxy_pass.location;
            runtime_location.pattern = location.pattern;
            runtime_location.matcher_pattern = compile_location_pattern(location);
            runtime_location.default_host_header = std::move(default_host_header);
            runtime_location.connect_timeout =
                resolve_timeout(location.proxy.connect_timeout, inherited_connect, kDefaultConnectTimeout);
            runtime_location.read_timeout =
                resolve_timeout(location.proxy.read_timeout, inherited_read, kDefaultReadTimeout);
            runtime_location.send_timeout =
                resolve_timeout(location.proxy.send_timeout, inherited_send, kDefaultSendTimeout);
            runtime_location.upstream_index = upstream_index;
            runtime_location.proxy_buffering = location.proxy.proxy_buffering;
            runtime_location.set_headers.reserve(location.proxy.set_headers.size());

            for (const auto &header : location.proxy.set_headers) {
                ProxyHeaderRuntime runtime_header;
                runtime_header.name = header.name;
                runtime_header.lowercase_name = header.lowercase_name;
                runtime_header.value = header.value;
                runtime_header.name_hash = fiber::http::http_header_name_hash(runtime_header.lowercase_name);
                runtime_location.host_header_overridden =
                    runtime_location.host_header_overridden || runtime_header.lowercase_name == "host";
                runtime_location.set_headers.push_back(std::move(runtime_header));
            }

            const std::uint32_t location_index = static_cast<std::uint32_t>(runtime_server.locations.size());
            try {
                matcher_builder.add_route(runtime_location.matcher_pattern, LocationRoutePayload{.location_index = location_index});
            } catch (const RoutePatternError &error) {
                return std::unexpected(make_error(location.proxy_pass.location, error.what()));
            }
            runtime_server.locations.push_back(std::move(runtime_location));
        }

        runtime_server.location_matcher = matcher_builder.build();
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
        runtime_listener.default_server_index = 0;
        runtime_listener.server_names.reserve(seen_server_names.size());

        for (std::uint32_t server_index = 0; server_index < runtime.servers.size(); ++server_index) {
            const auto &server = runtime.servers[server_index];
            for (const auto &name : server.server_names) {
                runtime_listener.server_names.push_back({
                    .name = name,
                    .server_index = server_index,
                });
            }
        }
        std::sort(runtime_listener.server_names.begin(), runtime_listener.server_names.end(),
                  [](const ServerNameRuntime &left, const ServerNameRuntime &right) {
                      return left.name < right.name;
                  });

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
