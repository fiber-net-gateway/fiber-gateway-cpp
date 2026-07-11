#ifndef FIBER_LITE_NGINX_PROXY_PROXY_HANDLER_H
#define FIBER_LITE_NGINX_PROXY_PROXY_HANDLER_H

#include "async/Task.h"

#include "../runtime/RuntimeConfig.h"

namespace fiber::http {
class HttpExchange;
}

namespace fiber::lite_nginx::runtime {
class DnsService;
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

    [[nodiscard]] fiber::async::Task<void> handle(fiber::http::HttpExchange &exchange,
                                                  const runtime::ListenerRuntime &listener,
                                                  const runtime::LocationRuntime &location) const;

private:
    upstream::UpstreamRegistry *upstreams_ = nullptr;
    upstream::ConnectionPool *pool_ = nullptr;
    runtime::DnsService *dns_ = nullptr;
};

} // namespace fiber::lite_nginx::proxy

#endif // FIBER_LITE_NGINX_PROXY_PROXY_HANDLER_H
