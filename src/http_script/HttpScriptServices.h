#ifndef FIBER_HTTP_SCRIPT_HTTP_SCRIPT_SERVICES_H
#define FIBER_HTTP_SCRIPT_HTTP_SCRIPT_SERVICES_H

#include <memory>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../http/Http1ClientConnection.h"
#include "../http/Http1ConnectionGroupKey.h"
#include "../net/IpAddress.h"
#include "../net/SocketAddress.h"
#include "../net/TlsOptions.h"

#include "HttpTarget.h"

namespace fiber::lite_nginx::upstream {
class ConnectionPool;
}

namespace fiber::lite_nginx::runtime {
class DnsService;
}

namespace fiber::http_script {

// Owns one upstream connection checkout (pooled or transient). Destroying it returns any pooled
// connection to the pool. acquire() on HttpScriptServices returns a fully *connected* connection:
// the unified path does pool lookup (hit -> reuse, zero DNS), and on miss connects a fresh
// connection (resolving DNS first when the key is a hostname). For name peers the dial target is
// resolved at connect time; the pooled identity stays the host name.
class HttpUpstreamConnection {
public:
    virtual ~HttpUpstreamConnection() = default;

    [[nodiscard]] virtual fiber::http::Http1ClientConnection &connection() noexcept = 0;
};

// App-provided bridge that resolves an HttpTargetSpec (named upstream or ad-hoc URL, the latter
// possibly requiring DNS) to a connected upstream connection, using the app's global pool.
// Implemented by lite_nginx (backed by ConnectionPool + per-loop DnsService + UpstreamRegistry).
class HttpScriptServices {
public:
    virtual ~HttpScriptServices() = default;

    // Returns a connected connection holder for the target, or an IoErr. The returned object owns
    // the pool lease for its lifetime.
    [[nodiscard]] virtual fiber::async::Task<fiber::common::IoResult<std::unique_ptr<HttpUpstreamConnection>>>
    acquire(const HttpTargetSpec &target, std::chrono::milliseconds connect_timeout) noexcept = 0;
};

} // namespace fiber::http_script

#endif // FIBER_HTTP_SCRIPT_HTTP_SCRIPT_SERVICES_H
