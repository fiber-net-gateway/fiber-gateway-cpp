#ifndef FIBER_LITE_NGINX_PROXY_PROXY_HANDLER_H
#define FIBER_LITE_NGINX_PROXY_PROXY_HANDLER_H

#include "async/Task.h"

#include "../runtime/RuntimeConfig.h"

namespace fiber::http {
class HttpExchange;
}

namespace fiber::lite_nginx::upstream {
class UpstreamRegistry;
}

namespace fiber::lite_nginx::proxy {

class ProxyHandler {
public:
    explicit ProxyHandler(upstream::UpstreamRegistry &upstreams) noexcept : upstreams_(&upstreams) {}

    [[nodiscard]] fiber::async::Task<void> handle(fiber::http::HttpExchange &exchange,
                                                  const runtime::LocationRuntime &location) const;

private:
    upstream::UpstreamRegistry *upstreams_ = nullptr;
};

} // namespace fiber::lite_nginx::proxy

#endif // FIBER_LITE_NGINX_PROXY_PROXY_HANDLER_H
