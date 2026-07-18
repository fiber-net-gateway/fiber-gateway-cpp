#include "RuntimeBuilder.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "common/util/RoutePathMatcher.h"
#include "http/HeaderMap.h"
#include "http/HttpHeaderHash.h"
#include "http_script/HttpScriptLib.h"
#include "http_script/RouteScriptExtension.h"
#include "logging/AccessLogScriptExtension.h"
#include "net/IpAddress.h"
#include "script/ScriptCompiler.h"

namespace fiber::lite_nginx::runtime {
namespace {

constexpr std::chrono::milliseconds kDefaultConnectTimeout{10000};
constexpr std::chrono::milliseconds kDefaultReadTimeout{30000};
constexpr std::chrono::milliseconds kDefaultSendTimeout{30000};
constexpr std::uint8_t kSkipHeaderValue = 1;

enum class ScriptCompileScope : std::uint8_t {
    RouteScript,
    ProxyHeaderTemplate,
    AccessLogTemplate,
};

using fiber::util::RoutePatternError;

RuntimeError make_error(const config::SourceLocation &location, std::string message) {
    return RuntimeError{
            .message = std::move(message),
            .location = location,
    };
}

// Reads a script file and compiles it against the runtime's StdLibrary. Host-call function
// pointers and userdata are copied into the compiled script; extension userdata is owned by
// RuntimeConfig's shared extension contexts.
std::expected<std::shared_ptr<fiber::script::Script>, RuntimeError>
compile_script_file(fiber::script::Library &library, const std::string &path, const config::SourceLocation &loc) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::unexpected(make_error(loc, "script_file not found: " + path));
    }
    std::string source((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    auto compiled = fiber::script::compile_script(library, source);
    if (!compiled) {
        return std::unexpected(make_error(loc, "script compile error: " + std::string(compiled.error().message)));
    }
    return std::make_shared<fiber::script::Script>(std::move(*compiled));
}

void ensure_script_library(RuntimeConfig &runtime) {
    if (runtime.script_library) {
        return;
    }
    runtime.script_library = std::make_shared<fiber::script::std_lib::StdLibrary>();
    fiber::http_script::register_http_functions_to_lib(*runtime.script_library);
    runtime.route_script_extension = std::make_shared<fiber::http_script::RouteScriptExtension>();
    runtime.access_log_script_extension = std::make_shared<logging::AccessLogScriptExtension>();
    runtime.script_library->add_ext_ops(runtime.access_log_script_extension.get(),
                                        logging::AccessLogScriptExtension::ops());
    runtime.script_library->add_ext_ops(runtime.route_script_extension.get(),
                                        fiber::http_script::RouteScriptExtension::ops());
}

void set_script_compile_context(RuntimeConfig &runtime, const std::vector<std::string> &path_var_names,
                                ScriptCompileScope scope) {
    runtime.route_script_extension->set_compile_path_vars(path_var_names);
    runtime.route_script_extension->set_http_directives_enabled(scope == ScriptCompileScope::RouteScript);
    runtime.access_log_script_extension->set_compile_enabled(scope == ScriptCompileScope::AccessLogTemplate);
}

std::expected<AccessLogId, RuntimeError> compile_access_log(RuntimeConfig &runtime,
                                                            const std::optional<config::AccessLogConfig> &access_log,
                                                            AccessLogId inherited,
                                                            const std::vector<std::string> &path_var_names) {
    if (!access_log) {
        return inherited;
    }
    if (access_log->kind == config::AccessLogKind::Off) {
        return kDisabledAccessLog;
    }
    if (runtime.access_logs.size() >= kDisabledAccessLog) {
        return std::unexpected(make_error(access_log->location, "too many access_log definitions"));
    }

    AccessLogRuntime compiled_log;
    compiled_log.location = access_log->location;
    compiled_log.logger_name = access_log->logger_name;
    if (access_log->message_template.find("${") == std::string::npos) {
        compiled_log.literal_message = access_log->message_template;
    } else {
        ensure_script_library(runtime);
        set_script_compile_context(runtime, path_var_names, ScriptCompileScope::AccessLogTemplate);
        auto compiled = fiber::script::compile_template_string(*runtime.script_library, access_log->message_template);
        if (!compiled) {
            return std::unexpected(
                    make_error(access_log->location, "access_log template compile error: " + compiled.error().message));
        }
        if (compiled->contains_async()) {
            return std::unexpected(make_error(access_log->location, "access_log template must be synchronous"));
        }
        compiled_log.template_script = std::make_shared<fiber::script::Script>(std::move(*compiled));
    }

    const AccessLogId id = static_cast<AccessLogId>(runtime.access_logs.size());
    runtime.access_logs.push_back(std::move(compiled_log));
    return id;
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

std::string make_http3_alt_svc(std::uint16_t port) {
    std::string value = "h3=\":";
    value.append(std::to_string(port));
    value.append("\"; ma=86400");
    return value;
}

std::string direct_upstream_key(std::string_view host, std::uint16_t port) {
    std::string key(host);
    key.push_back(':');
    key.append(std::to_string(port));
    return key;
}

std::chrono::milliseconds resolve_timeout(const std::optional<std::chrono::milliseconds> &override_value,
                                          std::chrono::milliseconds inherited, std::chrono::milliseconds fallback) {
    if (override_value.has_value()) {
        return *override_value;
    }
    if (inherited.count() > 0) {
        return inherited;
    }
    return fallback;
}

fiber::http::HeaderMap<std::uint8_t> make_default_skip_headers() {
    fiber::http::HeaderMap<std::uint8_t> headers;
    headers.insert("connection", fiber::http::http_header_name_hash("connection"), kSkipHeaderValue);
    headers.insert("keep-alive", fiber::http::http_header_name_hash("keep-alive"), kSkipHeaderValue);
    headers.insert("proxy-connection", fiber::http::http_header_name_hash("proxy-connection"), kSkipHeaderValue);
    headers.insert("transfer-encoding", fiber::http::http_header_name_hash("transfer-encoding"), kSkipHeaderValue);
    headers.insert("upgrade", fiber::http::http_header_name_hash("upgrade"), kSkipHeaderValue);
    headers.insert("te", fiber::http::http_header_name_hash("te"), kSkipHeaderValue);
    headers.insert("trailer", fiber::http::http_header_name_hash("trailer"), kSkipHeaderValue);
    headers.insert("proxy-authenticate", fiber::http::http_header_name_hash("proxy-authenticate"), kSkipHeaderValue);
    headers.insert("proxy-authorization", fiber::http::http_header_name_hash("proxy-authorization"), kSkipHeaderValue);
    headers.insert("host", fiber::http::http_header_name_hash("host"), kSkipHeaderValue);
    return headers;
}

std::expected<UpstreamPeerRuntime, RuntimeError> make_peer_runtime(const config::SourceLocation &location,
                                                                   std::string host, std::uint16_t port,
                                                                   std::uint32_t weight, bool tls) {
    const auto scheme = tls ? fiber::http::Http1ConnectionGroupKey::Scheme::Https
                            : fiber::http::Http1ConnectionGroupKey::Scheme::Http;

    UpstreamPeerRuntime peer;
    peer.host = host;
    peer.port = port;
    peer.weight = weight;

    fiber::net::IpAddress ip;
    if (fiber::net::IpAddress::parse(host, ip)) {
        // IP-literal peer: config-time dial target, no runtime DNS.
        peer.ip = ip;
        peer.address = fiber::net::SocketAddress(ip, port);
        peer.connection_key = fiber::http::Http1ConnectionGroupKey::from_ip(ip, port, scheme);
        return peer;
    }

    // Hostname peer: pool identity is the name; the dial target is resolved at runtime
    // via DnsService on the worker loop that needs a fresh connection.
    auto key = fiber::http::Http1ConnectionGroupKey::from_name(host, port, scheme);
    if (!key) {
        return std::unexpected(make_error(location, "upstream host name too long: " + host));
    }
    peer.connection_key = std::move(*key);
    return peer;
}

struct LocationRoutePayload {
    std::uint32_t location_index = 0;
};

struct LocationRouteDefiner {
    // When non-null, add_path_var_definer appends each path variable name here. Set per
    // location before add_route so the definer (shared across locations) writes into the
    // current location's name set; the matcher adds names in pattern order (idx 0,1,2,...).
    std::vector<std::string> *path_var_names_out = nullptr;

    void add_path_var_definer(LocationRoutePayload &, std::string_view name, std::uint32_t) {
        if (path_var_names_out != nullptr) {
            path_var_names_out->emplace_back(name);
        }
    }

    std::uint32_t on_route_mount(std::uint32_t, std::string_view, LocationRoutePayload &payload) {
        return payload.location_index;
    }
};

} // namespace

std::expected<RuntimeConfig, RuntimeError> RuntimeBuilder::build(const config::MainConfig &config) {
    RuntimeConfig runtime;
    runtime.worker_processes = config.worker_processes;
    const std::vector<std::string> no_path_vars;
    auto http_access_log = compile_access_log(runtime, config.http.access_log, kDisabledAccessLog, no_path_vars);
    if (!http_access_log) {
        return std::unexpected(http_access_log.error());
    }
    runtime.access_log = *http_access_log;
    runtime.connection_pool.keepalive_size = config.http.connection_pool.keepalive_size;
    runtime.connection_pool.keepalive_timeout = config.http.connection_pool.keepalive_timeout;
    runtime.connection_pool.max_idle_total = config.http.connection_pool.max_idle_total;
    runtime.connection_pool.initial_group_capacity = config.http.connection_pool.initial_group_capacity;
    // Resolve PoolSteal::Auto to a concrete choice at build time: steal across worker loops only
    // when there is more than one worker (single-worker stealable is dead cross-loop code).
    switch (config.http.connection_pool.steal) {
        case config::PoolSteal::On:
            runtime.connection_pool.steal = true;
            break;
        case config::PoolSteal::Off:
            runtime.connection_pool.steal = false;
            break;
        case config::PoolSteal::Auto:
            runtime.connection_pool.steal = runtime.worker_processes > 1;
            break;
    }
    runtime.upstreams.reserve(config.http.upstreams.size());
    runtime.servers.reserve(config.http.servers.size());
    runtime.listeners.reserve(config.http.listens.size());

    std::unordered_map<std::string, std::uint32_t> upstream_indices;
    upstream_indices.reserve(config.http.upstreams.size() * 2 + 4);

    for (const auto &upstream: config.http.upstreams) {
        UpstreamRuntime runtime_upstream;
        runtime_upstream.name = upstream.name;
        runtime_upstream.connect_timeout = upstream.connect_timeout.value_or(kDefaultConnectTimeout);
        runtime_upstream.read_timeout = upstream.read_timeout.value_or(kDefaultReadTimeout);
        runtime_upstream.send_timeout = upstream.send_timeout.value_or(kDefaultSendTimeout);
        runtime_upstream.peers.reserve(upstream.servers.size());

        for (const auto &server: upstream.servers) {
            auto peer_result =
                    make_peer_runtime(config::SourceLocation{}, server.host, server.port, server.weight, server.tls);
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

    for (const auto &server: config.http.servers) {
        ServerRuntime runtime_server;
        runtime_server.location = server.location;
        runtime_server.server_names = server.server_names;
        runtime_server.certificate = server.certificate;
        runtime_server.certificate_key = server.certificate_key;
        auto server_access_log = compile_access_log(runtime, server.access_log, runtime.access_log, no_path_vars);
        if (!server_access_log) {
            return std::unexpected(server_access_log.error());
        }
        runtime_server.access_log = *server_access_log;
        runtime_server.locations.reserve(server.locations.size());

        LocationRouteDefiner route_definer;
        fiber::util::RoutePathMatcher<std::uint32_t>::Builder<LocationRoutePayload, LocationRouteDefiner>
                matcher_builder(route_definer);

        for (const auto &name: server.server_names) {
            auto [it, inserted] = seen_server_names.emplace(name, server.location);
            if (!inserted) {
                return std::unexpected(make_error(
                        server.location, "duplicate server_name is not supported in lite-nginx runtime: " + name));
            }
        }

        for (const auto &location: server.locations) {
            if (location.kind == config::LocationKind::Script) {
                ensure_script_library(runtime);
                LocationRuntime runtime_location;
                runtime_location.location = location.location;
                runtime_location.pattern = location.pattern;
                const std::uint32_t location_index = static_cast<std::uint32_t>(runtime_server.locations.size());

                // Add the route first so the matcher extracts the pattern's path variable
                // names (e.g. /api/:id -> ["id"]) into path_var_names; the script is then
                // compiled with the shared route extension set to those names, so $path
                // references are validated at compile time.
                std::vector<std::string> path_var_names;
                route_definer.path_var_names_out = &path_var_names;
                try {
                    matcher_builder.add_route(runtime_location.pattern,
                                              LocationRoutePayload{.location_index = location_index});
                } catch (const RoutePatternError &error) {
                    route_definer.path_var_names_out = nullptr;
                    return std::unexpected(make_error(location.location, error.what()));
                }
                route_definer.path_var_names_out = nullptr;

                auto location_access_log =
                        compile_access_log(runtime, location.access_log, runtime_server.access_log, path_var_names);
                if (!location_access_log) {
                    return std::unexpected(location_access_log.error());
                }
                runtime_location.access_log = *location_access_log;

                set_script_compile_context(runtime, path_var_names, ScriptCompileScope::RouteScript);
                auto script = compile_script_file(*runtime.script_library, location.script_file, location.location);
                if (!script) {
                    return std::unexpected(script.error());
                }
                runtime_location.script = std::move(*script);
                runtime_server.locations.push_back(std::move(runtime_location));
                continue;
            }

            std::uint32_t upstream_index = 0;
            std::string default_host_header;
            std::chrono::milliseconds inherited_connect = kDefaultConnectTimeout;
            std::chrono::milliseconds inherited_read = kDefaultReadTimeout;
            std::chrono::milliseconds inherited_send = kDefaultSendTimeout;

            if (location.proxy_pass.kind == config::ProxyPassKind::NamedUpstream) {
                auto it = upstream_indices.find(location.proxy_pass.upstream_name);
                if (it == upstream_indices.end()) {
                    return std::unexpected(make_error(location.proxy_pass.location,
                                                      "proxy_pass references unknown upstream in runtime: " +
                                                              location.proxy_pass.upstream_name));
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
                    auto peer_result = make_peer_runtime(location.proxy_pass.location, location.proxy_pass.host,
                                                         location.proxy_pass.port, 1, false);
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
            runtime_location.default_host_header = std::move(default_host_header);
            runtime_location.connect_timeout =
                    resolve_timeout(location.proxy.connect_timeout, inherited_connect, kDefaultConnectTimeout);
            runtime_location.read_timeout =
                    resolve_timeout(location.proxy.read_timeout, inherited_read, kDefaultReadTimeout);
            runtime_location.send_timeout =
                    resolve_timeout(location.proxy.send_timeout, inherited_send, kDefaultSendTimeout);
            runtime_location.upstream_index = upstream_index;
            runtime_location.proxy_buffering = location.proxy.proxy_buffering;
            runtime_location.skip_headers = make_default_skip_headers();
            // Add the route first so the matcher extracts the pattern's path variable names
            // (e.g. /api/:id -> ["id"]) into path_var_names; template header values are then
            // compiled with the shared route extension set to those names, so $path references
            // validate at compile time. Mirrors the script-location flow.
            const std::uint32_t location_index = static_cast<std::uint32_t>(runtime_server.locations.size());
            std::vector<std::string> path_var_names;
            route_definer.path_var_names_out = &path_var_names;
            try {
                matcher_builder.add_route(runtime_location.pattern,
                                          LocationRoutePayload{.location_index = location_index});
            } catch (const RoutePatternError &error) {
                route_definer.path_var_names_out = nullptr;
                return std::unexpected(make_error(location.proxy_pass.location, error.what()));
            }
            route_definer.path_var_names_out = nullptr;

            auto location_access_log =
                    compile_access_log(runtime, location.access_log, runtime_server.access_log, path_var_names);
            if (!location_access_log) {
                return std::unexpected(location_access_log.error());
            }
            runtime_location.access_log = *location_access_log;

            // Compile ${...} template header values with the shared route extension. Static-only
            // locations pay no script compilation cost.
            bool has_template_header = false;
            for (const auto &header: location.proxy.set_headers) {
                if (header.is_template) {
                    has_template_header = true;
                    break;
                }
            }
            if (has_template_header) {
                ensure_script_library(runtime);
                set_script_compile_context(runtime, path_var_names, ScriptCompileScope::ProxyHeaderTemplate);
            }

            runtime_location.set_headers.reserve(location.proxy.set_headers.size());
            for (const auto &header: location.proxy.set_headers) {
                ProxyHeaderRuntime runtime_header;
                runtime_header.name = header.name;
                runtime_header.lowercase_name = header.lowercase_name;
                runtime_header.name_hash = fiber::http::http_header_name_hash(runtime_header.lowercase_name);
                runtime_location.host_header_overridden =
                        runtime_location.host_header_overridden || runtime_header.lowercase_name == "host";
                runtime_location.skip_headers.insert(runtime_header.lowercase_name, runtime_header.name_hash,
                                                     kSkipHeaderValue);
                if (header.is_template) {
                    auto compiled = fiber::script::compile_template_string(*runtime.script_library, header.value);
                    if (!compiled) {
                        return std::unexpected(
                                make_error(location.location,
                                           "proxy_set_header template compile error: " + compiled.error().message));
                    }
                    if (compiled->contains_async()) {
                        return std::unexpected(make_error(
                                location.location, "proxy_set_header template must be synchronous: " + header.name));
                    }
                    runtime_header.template_script = std::make_shared<fiber::script::Script>(std::move(*compiled));
                }
                runtime_header.value = header.value; // literal value, or template source (diagnostics)
                runtime_location.set_headers.push_back(std::move(runtime_header));
            }
            runtime_server.locations.push_back(std::move(runtime_location));
        }

        runtime_server.location_matcher = matcher_builder.build();
        runtime.servers.push_back(std::move(runtime_server));
    }

    std::unordered_map<std::string, config::SourceLocation> seen_listeners;
    seen_listeners.reserve(config.http.listens.size());
    for (const auto &listen: config.http.listens) {
        auto key = listener_key(listen);
        auto [it, inserted] = seen_listeners.emplace(key, listen.location);
        if (!inserted) {
            return std::unexpected(
                    make_error(listen.location, "duplicate listen is not supported in lite-nginx runtime: " + key));
        }

        ListenerRuntime runtime_listener;
        runtime_listener.location = listen.location;
        runtime_listener.host = listen.host;
        runtime_listener.port = listen.port;
        runtime_listener.has_host = listen.has_host;
        runtime_listener.tls = listen.tls;
        runtime_listener.http3 = listen.http3;
        if (runtime_listener.http3) {
            runtime_listener.http3_alt_svc = make_http3_alt_svc(runtime_listener.port);
        }
        runtime_listener.default_server_index = 0;
        runtime_listener.server_names.reserve(seen_server_names.size());

        for (std::uint32_t server_index = 0; server_index < runtime.servers.size(); ++server_index) {
            const auto &server = runtime.servers[server_index];
            for (const auto &name: server.server_names) {
                runtime_listener.server_names.push_back({
                        .name = name,
                        .server_index = server_index,
                });
            }
        }
        std::sort(runtime_listener.server_names.begin(), runtime_listener.server_names.end(),
                  [](const ServerNameRuntime &left, const ServerNameRuntime &right) { return left.name < right.name; });

        if (listen.tls) {
            const auto &default_server = config.http.servers.front();
            runtime_listener.default_certificate = default_server.certificate;
            runtime_listener.default_certificate_key = default_server.certificate_key;

            std::size_t identity_count = 0;
            for (const auto &server: config.http.servers) {
                identity_count += server.server_names.size();
            }
            runtime_listener.tls_identities.reserve(identity_count);
            for (const auto &server: config.http.servers) {
                for (const auto &name: server.server_names) {
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
