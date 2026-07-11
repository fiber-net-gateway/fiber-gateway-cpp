#ifndef FIBER_LITE_NGINX_RUNTIME_HTTP_SCRIPT_SERVICES_H
#define FIBER_LITE_NGINX_RUNTIME_HTTP_SCRIPT_SERVICES_H

#include <memory>

#include "async/Task.h"
#include "common/IoError.h"
#include "http_script/HttpScriptServices.h"
#include "http_script/HttpTarget.h"

#include "DnsService.h"

namespace fiber::lite_nginx::upstream {
class UpstreamRegistry;
}

namespace fiber::lite_nginx::runtime {

// HttpScriptServices backed by the app's global UpstreamRegistry + per-loop DnsService.
//   - Upstream target: weighted peer selection + global pool (acquire_by_name).
//   - Url target: IP-literal hosts connect directly; hostnames are resolved via DnsService on the
//     calling worker loop, then connect via the global pool keyed by the resolved peer.
class HttpScriptServicesImpl : public fiber::http_script::HttpScriptServices {
public:
    HttpScriptServicesImpl(upstream::UpstreamRegistry &upstreams, DnsService &dns) noexcept;

    [[nodiscard]] fiber::async::Task<
            fiber::common::IoResult<std::unique_ptr<fiber::http_script::HttpUpstreamConnection>>>
    acquire(const fiber::http_script::HttpTargetSpec &target) noexcept override;

private:
    upstream::UpstreamRegistry *upstreams_;
    DnsService *dns_;
};

} // namespace fiber::lite_nginx::runtime

#endif // FIBER_LITE_NGINX_RUNTIME_HTTP_SCRIPT_SERVICES_H
