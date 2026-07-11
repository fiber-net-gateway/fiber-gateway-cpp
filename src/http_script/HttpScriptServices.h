#ifndef FIBER_HTTP_SCRIPT_HTTP_SCRIPT_SERVICES_H
#define FIBER_HTTP_SCRIPT_HTTP_SCRIPT_SERVICES_H

#include <memory>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../http/Http1ClientConnection.h"
#include "../net/SocketAddress.h"
#include "../net/TlsOptions.h"

#include "HttpTarget.h"

namespace fiber::http_script {

// Owns one upstream connection checkout (pooled or transient). Destroying it returns any pooled
// connection to the pool. The script host functions drive the connection lifecycle via the
// accessors below, mirroring how ProxyHandler uses UpstreamRegistry::ConnectionHandle.
class HttpUpstreamConnection {
public:
    virtual ~HttpUpstreamConnection() = default;

    [[nodiscard]] virtual fiber::net::SocketAddress peer_addr() const noexcept = 0;
    [[nodiscard]] virtual fiber::net::TlsOptions tls() const noexcept = 0;
    // True when the connection came from the keepalive pool (has a lease). False => the caller
    // must construct a transient Http1ClientConnection itself.
    [[nodiscard]] virtual bool pooled() const noexcept = 0;
    // Existing idle pooled connection, or nullptr if the pool had none (caller should emplace).
    [[nodiscard]] virtual fiber::http::Http1ClientConnection *connection() noexcept = 0;
    // Create + insert a fresh connection into the pool. Only valid when pooled() && !connection().
    [[nodiscard]] virtual fiber::common::IoResult<fiber::http::Http1ClientConnection *>
    emplace_connection(fiber::http::Http1ClientConnectionOptions options) noexcept = 0;
};

// App-provided bridge that resolves an HttpTargetSpec (named upstream or ad-hoc URL, the latter
// possibly requiring DNS) to a checked-out upstream connection, using the app's global pool.
// Implemented by lite_nginx (backed by UpstreamRegistry + per-loop DnsResolver).
class HttpScriptServices {
public:
    virtual ~HttpScriptServices() = default;

    // Returns a connection holder for the target, or an IoErr. The returned object owns the
    // pool lease for its lifetime.
    [[nodiscard]] virtual fiber::async::Task<fiber::common::IoResult<std::unique_ptr<HttpUpstreamConnection>>>
    acquire(const HttpTargetSpec &target) noexcept = 0;
};

} // namespace fiber::http_script

#endif // FIBER_HTTP_SCRIPT_HTTP_SCRIPT_SERVICES_H
