#ifndef FIBER_LITE_NGINX_CONFIG_CONFIG_H
#define FIBER_LITE_NGINX_CONFIG_CONFIG_H

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "Ast.h"

namespace fiber::lite_nginx::config {

struct HeaderOverride {
    std::string name;
    std::string lowercase_name;
    std::string value;
};

struct ProxySettings {
    std::optional<std::chrono::milliseconds> connect_timeout;
    std::optional<std::chrono::milliseconds> read_timeout;
    std::optional<std::chrono::milliseconds> send_timeout;
    std::vector<HeaderOverride> set_headers;
    bool proxy_buffering = false;
};

struct ListenAddress {
    std::string host;
    std::uint16_t port = 0;
    bool has_host = false;
    bool tls = false;
};

struct UpstreamServerConfig {
    std::string host;
    std::uint16_t port = 0;
};

struct UpstreamConfig {
    std::string name;
    std::vector<UpstreamServerConfig> servers;
    std::size_t keepalive = 0;
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

enum class LocationMatchKind : unsigned char {
    Prefix,
    Exact,
};

struct LocationConfig {
    LocationMatchKind match_kind = LocationMatchKind::Prefix;
    std::string pattern;
    ProxyPassTarget proxy_pass;
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
};

struct MainConfig {
    std::size_t worker_processes = 1;
    HttpConfig http;
};

} // namespace fiber::lite_nginx::config

#endif // FIBER_LITE_NGINX_CONFIG_CONFIG_H
