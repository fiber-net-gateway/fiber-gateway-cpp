#ifndef FIBER_LITE_NGINX_PROXY_PROXY_HANDLER_H
#define FIBER_LITE_NGINX_PROXY_PROXY_HANDLER_H

#include <string_view>
#include <utility>
#include <vector>

#include "async/Task.h"

#include "../runtime/RuntimeConfig.h"

namespace fiber::http {
class HttpExchange;
}

namespace fiber::http_script {
class HttpScriptServices;
}

namespace fiber::lite_nginx::runtime {
class DnsService;
}

namespace fiber::lite_nginx::logging {
struct RequestLogContext;
}

namespace fiber::lite_nginx::upstream {
class ConnectionPool;
class UpstreamRegistry;
} // namespace fiber::lite_nginx::upstream

namespace fiber::lite_nginx::proxy {

class ProxyHandler {
public:
    ProxyHandler(upstream::UpstreamRegistry &upstreams, upstream::ConnectionPool &pool,
                 runtime::DnsService &dns) noexcept;

    // path_vars: route captures for this request (name/value pairs borrowing matcher text).
    // services: app-provided upstream services, attached to a per-request ScriptExchangeCtx so
    // template header values ($header.host etc.) resolve. Unused for static-header locations.
    [[nodiscard]] fiber::async::Task<void>
    handle(fiber::http::HttpExchange &exchange, const runtime::ListenerRuntime &listener,
           const runtime::LocationRuntime &location,
           const std::vector<std::pair<std::string_view, std::string_view>> &path_vars,
           fiber::http_script::HttpScriptServices *services, logging::RequestLogContext &log_context) const;

private:
    upstream::UpstreamRegistry *upstreams_ = nullptr;
    upstream::ConnectionPool *pool_ = nullptr;
    runtime::DnsService *dns_ = nullptr;
};

} // namespace fiber::lite_nginx::proxy

#endif // FIBER_LITE_NGINX_PROXY_PROXY_HANDLER_H
