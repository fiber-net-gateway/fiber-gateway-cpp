#ifndef FIBER_LITE_NGINX_UPSTREAM_UPSTREAM_CONNECTION_H
#define FIBER_LITE_NGINX_UPSTREAM_UPSTREAM_CONNECTION_H

#include <chrono>
#include <memory>
#include <utility>

#include "async/Task.h"
#include "common/IoError.h"
#include "http/Http1ClientConnection.h"
#include "http/Http1ConnectionGroupKey.h"
#include "net/IpAddress.h"
#include "net/SocketAddress.h"
#include "net/TlsOptions.h"

#include "ConnectionPool.h"

namespace fiber::lite_nginx::runtime {
class DnsService;
}

namespace fiber::lite_nginx::upstream {

// Result of acquire_and_connect: owns the pool lease (pooled case) or a transient connection
// (keepalive_size == 0 / no-pool case), and exposes the connected Http1ClientConnection.
// `conn` is always non-null on success. Move-only.
struct AcquiredUpstreamConnection {
    ConnectionPool::ConnectionLease lease{};
    std::unique_ptr<fiber::http::Http1ClientConnection> transient;
    fiber::http::Http1ClientConnection *conn = nullptr;
};

// Unified acquire + connect path. Resolves the peer identity to a connected Http1ClientConnection:
//   1. pool.acquire(key) -> hit (has_connection) => reuse, zero DNS.
//   2. miss => resolve the dial target: IP-key peers use the key's ip directly; name-key peers
//      resolve via DnsService on the calling worker loop, then dial that IP.
//   3. emplace_connection(opts) + connect() (pooled), or construct a transient connection + connect()
//      when no pool is configured (keepalive_size == 0).
// `tls_server_name` is forwarded to TlsOptions.server_name for HTTPS keys (SNI); ignored for HTTP.
[[nodiscard]] fiber::async::Task<fiber::common::IoResult<AcquiredUpstreamConnection>>
acquire_and_connect(ConnectionPool &pool, fiber::lite_nginx::runtime::DnsService &dns,
                    const fiber::http::Http1ConnectionGroupKey &key, std::string_view tls_server_name,
                    std::chrono::milliseconds connect_timeout) noexcept;

} // namespace fiber::lite_nginx::upstream

#endif // FIBER_LITE_NGINX_UPSTREAM_UPSTREAM_CONNECTION_H
