#ifndef FIBER_LITE_NGINX_RUNTIME_RUNTIME_CONFIG_H
#define FIBER_LITE_NGINX_RUNTIME_RUNTIME_CONFIG_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../config/Ast.h"
#include "common/util/RoutePathMatcher.h"
#include "http/HeaderMap.h"
#include "http/Http1ConnectionGroupKey.h"
#include "net/IpAddress.h"
#include "net/SocketAddress.h"
#include "script/Script.h"
#include "script/std/StdLibrary.h"

namespace fiber::http_script {
class RouteScriptLibrary;
}

namespace fiber::lite_nginx::runtime {

struct RuntimeError {
    std::string message;
    config::SourceLocation location;
};

// Global keepalive connection pool shared across all upstreams. Configured once under
// http.connection_pool; one StealableHttp1ConnectionPoolSet keyed by peer (Http1ConnectionGroupKey).
struct ConnectionPoolRuntime {
    std::size_t keepalive_size = 0;
    std::chrono::milliseconds keepalive_timeout{30000};
};

struct TlsIdentityRuntime {
    std::string server_name;
    std::string certificate;
    std::string certificate_key;
};

struct ProxyHeaderRuntime {
    std::string name;
    std::string lowercase_name;
    std::string value;
    std::uint64_t name_hash = 0;
};

struct UpstreamPeerRuntime {
    std::string host;
    std::uint16_t port = 0;
    std::uint32_t weight = 1;
    fiber::net::IpAddress ip{};
    fiber::net::SocketAddress address{};
    std::optional<fiber::http::Http1ConnectionGroupKey> connection_key{};
};

struct UpstreamRuntime {
    std::string name;
    std::vector<UpstreamPeerRuntime> peers;
    std::chrono::milliseconds connect_timeout{10000};
    std::chrono::milliseconds read_timeout{30000};
    std::chrono::milliseconds send_timeout{30000};
};

struct LocationRuntime {
    config::SourceLocation location;
    std::string pattern;
    std::string matcher_pattern;
    std::string default_host_header;
    fiber::http::HeaderMap<std::uint8_t> skip_headers;
    std::vector<ProxyHeaderRuntime> set_headers;
    std::chrono::milliseconds connect_timeout{10000};
    std::chrono::milliseconds read_timeout{30000};
    std::chrono::milliseconds send_timeout{30000};
    std::uint32_t upstream_index = 0;
    bool proxy_buffering = false;
    bool host_header_overridden = false;
    // Non-null when this location runs a script (kind == Script) instead of proxying.
    std::shared_ptr<fiber::script::Script> script;
    // The per-location route-scoped library the script was compiled against. Outlives the
    // script (HostCallable pointers are baked in); kept alive here alongside it.
    std::shared_ptr<fiber::http_script::RouteScriptLibrary> route_lib;
};

struct ServerRuntime {
    config::SourceLocation location;
    std::vector<std::string> server_names;
    std::string certificate;
    std::string certificate_key;
    std::vector<LocationRuntime> locations;
    fiber::util::RoutePathMatcher<std::uint32_t> location_matcher;
};

struct ServerNameRuntime {
    std::string name;
    std::uint32_t server_index = 0;
};

struct ListenerRuntime {
    config::SourceLocation location;
    std::string host;
    std::uint16_t port = 0;
    bool has_host = false;
    bool tls = false;
    bool http3 = false;
    std::string http3_alt_svc;
    std::string default_certificate;
    std::string default_certificate_key;
    std::vector<TlsIdentityRuntime> tls_identities;
    std::vector<ServerNameRuntime> server_names;
    std::uint32_t default_server_index = 0;
};

struct RuntimeConfig {
    std::size_t worker_processes = 1;
    std::vector<UpstreamRuntime> upstreams;
    std::vector<ServerRuntime> servers;
    std::vector<ListenerRuntime> listeners;
    ConnectionPoolRuntime connection_pool;
    // Shared across all script locations (one StdLibrary with the HTTP functions registered),
    // kept alive for the runtime's lifetime since compiled scripts bake in function pointers.
    std::shared_ptr<fiber::script::std_lib::StdLibrary> script_library;
};

} // namespace fiber::lite_nginx::runtime

#endif // FIBER_LITE_NGINX_RUNTIME_RUNTIME_CONFIG_H
