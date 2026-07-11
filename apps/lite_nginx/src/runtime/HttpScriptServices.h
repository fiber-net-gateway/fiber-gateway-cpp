#ifndef FIBER_LITE_NGINX_RUNTIME_HTTP_SCRIPT_SERVICES_H
#define FIBER_LITE_NGINX_RUNTIME_HTTP_SCRIPT_SERVICES_H

#include <chrono>
#include <memory>

#include "async/Task.h"
#include "common/IoError.h"
#include "http/Http1ClientConnection.h"
#include "http_script/HttpScriptServices.h"
#include "http_script/HttpTarget.h"

#include "DnsService.h"

namespace fiber::lite_nginx::upstream {
class ConnectionPool;
class UpstreamRegistry;
} // namespace fiber::lite_nginx::upstream

namespace fiber::lite_nginx::runtime {

// HttpScriptServices backed by the app's global ConnectionPool + DnsService + UpstreamRegistry.
//   - Upstream target: weighted peer selection (UpstreamRegistry) -> peer's connection_key ->
//     acquire_and_connect (pool hit reuses; miss connects, resolving DNS for name peers).
//   - Url target: builds a key from host:port:scheme (IP-literal or hostname), same pool path.
// Both share the single global pool keyed by peer identity.
class HttpScriptServicesImpl : public fiber::http_script::HttpScriptServices {
public:
    HttpScriptServicesImpl(upstream::UpstreamRegistry &upstreams, upstream::ConnectionPool &pool,
                           DnsService &dns) noexcept;

    [[nodiscard]] fiber::async::Task<
            fiber::common::IoResult<std::unique_ptr<fiber::http_script::HttpUpstreamConnection>>>
    acquire(const fiber::http_script::HttpTargetSpec &target,
            std::chrono::milliseconds connect_timeout) noexcept override;

private:
    upstream::UpstreamRegistry *upstreams_;
    upstream::ConnectionPool *pool_;
    DnsService *dns_;
};

} // namespace fiber::lite_nginx::runtime

#endif // FIBER_LITE_NGINX_RUNTIME_HTTP_SCRIPT_SERVICES_H
