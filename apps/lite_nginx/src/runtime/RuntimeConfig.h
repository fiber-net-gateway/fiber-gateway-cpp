#ifndef FIBER_LITE_NGINX_RUNTIME_RUNTIME_CONFIG_H
#define FIBER_LITE_NGINX_RUNTIME_RUNTIME_CONFIG_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../config/Ast.h"
#include "common/util/RoutePathMatcher.h"
#include "http/HeaderMap.h"
#include "http/Http1ConnectionGroupKey.h"
#include "http/HttpBodyPipe.h"
#include "net/IpAddress.h"
#include "net/SocketAddress.h"
#include "script/Script.h"
#include "script/std/StdLibrary.h"

namespace fiber::http_script {
class ConstPackage;
class RouteScriptExtension;
} // namespace fiber::http_script

namespace fiber::lite_nginx::logging {
class AccessLogScriptExtension;
}

namespace fiber::lite_nginx::runtime {

using AccessLogId = std::uint32_t;
inline constexpr AccessLogId kDisabledAccessLog = std::numeric_limits<AccessLogId>::max();

struct RuntimeError {
    std::string message;
    config::SourceLocation location;
};

struct AccessLogRuntime {
    config::SourceLocation location;
    std::string logger_name;
    // A source without interpolation uses literal_message directly.
    std::string literal_message;
    std::shared_ptr<fiber::script::Script> template_script;
    std::shared_ptr<const fiber::http_script::ConstPackage> const_package;
};

// Global keepalive connection pool shared across all upstreams. Configured once under
// http.connection_pool; keyed by peer (Http1ConnectionGroupKey). `steal` is the build-time
// resolution of PoolSteal::Auto (true when worker_processes > 1): true -> StealableHttp1ConnectionPoolSet
// (idle connections shared across worker loops), false -> LocalHttp1ConnectionPoolSet (per-loop).
struct ConnectionPoolRuntime {
    std::size_t keepalive_size = 32;
    std::chrono::milliseconds keepalive_timeout{30000};
    bool steal = false;
    std::size_t max_idle_total = 0; // 0 => derive (keepalive_size * 64)
    std::size_t initial_group_capacity = 0; // 0 => built-in default (16)
};

struct TlsIdentityRuntime {
    std::string server_name;
    std::string certificate;
    std::string certificate_key;
};

struct ProxyHeaderRuntime {
    std::string name;
    std::string lowercase_name;
    std::string value; // literal value (used when template_script == nullptr)
    std::uint64_t name_hash = 0;
    // Non-null when `value` is a ${...} template compiled at runtime-build; evaluated per
    // request against a ScriptExchangeCtx to produce the header value. Null => use `value`.
    std::shared_ptr<fiber::script::Script> template_script;
};

enum class RewritePathKind : std::uint8_t {
    Preserve,
    Literal,
    Template,
};

struct RewritePathRuntime {
    RewritePathKind kind = RewritePathKind::Preserve;
    std::string literal;
    std::shared_ptr<fiber::script::Script> template_script;
};

struct UpstreamPeerRuntime {
    std::string host;
    std::uint16_t port = 0;
    std::uint32_t weight = 1;
    fiber::net::IpAddress ip{}; // valid only when connection_key.is_ip()
    fiber::net::SocketAddress address{}; // IP peers: config-time dial target; name peers: filled at runtime after DNS
    std::optional<fiber::http::Http1ConnectionGroupKey> connection_key{};
};

struct UpstreamRuntime {
    std::string name;
    std::vector<UpstreamPeerRuntime> peers;
    std::chrono::milliseconds connect_timeout{10000};
    std::chrono::milliseconds read_timeout{60000};
    std::chrono::milliseconds send_timeout{60000};
};

struct ProxyBufferingRuntime {
    std::size_t buffer_size = fiber::http::kDefaultBodyPipeBufferSize;
    // Zero selects HttpBodyPipe's unbuffered mode.
    std::size_t low_water = fiber::http::kUnbufferedBodyPipeLowWater;

    [[nodiscard]] bool enabled() const noexcept { return low_water != fiber::http::kUnbufferedBodyPipeLowWater; }
};

struct LocationRuntime {
    config::SourceLocation location;
    // The location pattern, fed verbatim to RoutePathMatcher: a bare pattern matches exactly
    // that path, `:name` captures one segment, and `*name`/`*` is a trailing wildcard (so `/*`
    // is the catch-all and `/` matches only the root path).
    std::string pattern;
    std::string default_host_header;
    RewritePathRuntime rewrite_path;
    fiber::http::HeaderMap<std::uint8_t> skip_headers;
    std::vector<ProxyHeaderRuntime> set_headers;
    std::chrono::milliseconds connect_timeout{10000};
    std::chrono::milliseconds read_timeout{60000};
    std::chrono::milliseconds send_timeout{60000};
    ProxyBufferingRuntime buffering;
    std::uint32_t upstream_index = 0;
    AccessLogId access_log = kDisabledAccessLog;
    bool host_header_overridden = false;
    bool reuse_connection = true;
    bool close_on_client_abort = false;
    // Non-null when this location runs a script (kind == Script) instead of proxying.
    std::shared_ptr<fiber::script::Script> script;
    // Immutable constant layout shared by every script/template compiled for this location.
    std::shared_ptr<const fiber::http_script::ConstPackage> const_package;
};

struct ServerRuntime {
    config::SourceLocation location;
    std::vector<std::string> server_names;
    std::string certificate;
    std::string certificate_key;
    std::vector<LocationRuntime> locations;
    fiber::util::RoutePathMatcher<std::uint32_t> location_matcher;
    AccessLogId access_log = kDisabledAccessLog;
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
    AccessLogId access_log = kDisabledAccessLog;
    std::vector<AccessLogRuntime> access_logs;
    std::vector<UpstreamRuntime> upstreams;
    std::vector<ServerRuntime> servers;
    std::vector<ListenerRuntime> listeners;
    ConnectionPoolRuntime connection_pool;
    // Shared across serial script compilation in this runtime. Compiled constant userdata is
    // owned by the immutable package attached to each script-bearing runtime object.
    std::shared_ptr<fiber::script::std_lib::StdLibrary> script_library;
    std::shared_ptr<fiber::http_script::RouteScriptExtension> route_script_extension;
    std::shared_ptr<fiber::lite_nginx::logging::AccessLogScriptExtension> access_log_script_extension;
};

} // namespace fiber::lite_nginx::runtime

#endif // FIBER_LITE_NGINX_RUNTIME_RUNTIME_CONFIG_H
