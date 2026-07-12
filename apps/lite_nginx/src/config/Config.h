#ifndef FIBER_LITE_NGINX_CONFIG_CONFIG_H
#define FIBER_LITE_NGINX_CONFIG_CONFIG_H

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "Ast.h"

namespace fiber::lite_nginx::config {

// Global keepalive connection pool shared across all upstreams (one pool, keyed by peer).
// `steal` controls cross-loop connection sharing: Auto resolves at runtime-build time to
// (worker_processes > 1); On uses the stealable pool (idle connections shared across worker
// loops); Off uses per-loop local pools (no cross-loop reuse).
enum class PoolSteal : unsigned char {
    Auto,
    On,
    Off,
};

struct ConnectionPoolConfig {
    std::size_t keepalive_size = 0; // max idle connections per peer group
    std::chrono::milliseconds keepalive_timeout{30000}; // idle timeout
    PoolSteal steal = PoolSteal::Auto;
    // 0 => derive from keepalive_size (keepalive_size * 64, the historical default). Caps the
    // total idle connections across all peer groups in one pool.
    std::size_t max_idle_total = 0;
    // 0 => use the built-in default (16). Initial slot capacity of the per-group hash table.
    std::size_t initial_group_capacity = 0;
};

struct HeaderOverride {
    std::string name;
    std::string lowercase_name;
    std::string value;
    // True when `value` contains ${...} and is compiled to a template script at runtime-build
    // (evaluated per request). False => `value` is a literal copied as-is.
    bool is_template = false;
};

struct ProxySettings {
    std::optional<std::chrono::milliseconds> connect_timeout;
    std::optional<std::chrono::milliseconds> read_timeout;
    std::optional<std::chrono::milliseconds> send_timeout;
    std::vector<HeaderOverride> set_headers;
    bool proxy_buffering = false;
};

struct ListenAddress {
    SourceLocation location;
    std::string host;
    std::uint16_t port = 0;
    bool has_host = false;
    bool tls = false;
    bool http3 = false;
};

struct UpstreamServerConfig {
    std::string host;
    std::uint16_t port = 0;
    std::uint32_t weight = 1;
    bool tls = false; // derived from the per-server scheme prefix (https:// = true)
};

struct UpstreamConfig {
    std::string name;
    std::vector<UpstreamServerConfig> servers;
    std::optional<std::chrono::milliseconds> connect_timeout;
    std::optional<std::chrono::milliseconds> read_timeout;
    std::optional<std::chrono::milliseconds> send_timeout;
};

enum class ProxyPassKind : unsigned char {
    NamedUpstream,
    Direct,
};

struct ProxyPassTarget {
    ProxyPassKind kind = ProxyPassKind::NamedUpstream;
    std::string raw;
    std::string upstream_name;
    std::string host;
    std::uint16_t port = 0;
    SourceLocation location;
};

enum class LocationKind : unsigned char {
    Proxy,
    Script,
};

struct LocationConfig {
    SourceLocation location;
    // The url pattern handed verbatim to RoutePathMatcher: a bare static pattern matches
    // exactly that path; `:name` captures one segment; `*name`/`*` is a trailing wildcard.
    // No `=` exact modifier and no auto prefix-shorthand -- the pattern is the match spec.
    std::string pattern;
    LocationKind kind = LocationKind::Proxy;
    ProxyPassTarget proxy_pass;
    std::string script_file;
    ProxySettings proxy;
};

struct ServerConfig {
    SourceLocation location;
    std::vector<std::string> server_names;
    std::string certificate;
    std::string certificate_key;
    ProxySettings proxy_defaults;
    std::vector<LocationConfig> locations;
};

struct HttpConfig {
    std::vector<ListenAddress> listens;
    std::vector<UpstreamConfig> upstreams;
    std::vector<ServerConfig> servers;
    ConnectionPoolConfig connection_pool;
};

struct MainConfig {
    std::size_t worker_processes = 1;
    HttpConfig http;
};

} // namespace fiber::lite_nginx::config

#endif // FIBER_LITE_NGINX_CONFIG_CONFIG_H
