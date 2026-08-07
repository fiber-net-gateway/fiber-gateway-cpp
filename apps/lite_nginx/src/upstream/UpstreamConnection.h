#ifndef FIBER_LITE_NGINX_UPSTREAM_UPSTREAM_CONNECTION_H
#define FIBER_LITE_NGINX_UPSTREAM_UPSTREAM_CONNECTION_H

#include <chrono>
#include <cstdint>
#include <memory>
#include <utility>

#include <fiber/async/Task.h>
#include <fiber/common/IoError.h>
#include <fiber/http/Http1ClientConnection.h>
#include <fiber/http/Http1ConnectionGroupKey.h>
#include <fiber/net/IpAddress.h>
#include <fiber/net/SocketAddress.h>
#include <fiber/net/TlsOptions.h>

#include "ConnectionPool.h"

namespace fiber::lite_nginx::runtime {
class DnsService;
}

namespace fiber::lite_nginx::upstream {

enum class ConnectionReusePolicy : std::uint8_t {
    Pooled,
    Transient,
};

// Result of acquire_and_connect: owns the pool lease (pooled case) or a transient connection
// (keepalive_size == 0 or an explicit Transient policy), and exposes the connected
// Http1ClientConnection.
// `conn` is always non-null on success. Move-only.
struct AcquiredUpstreamConnection {
    ConnectionPool::ConnectionLease lease{};
    std::unique_ptr<fiber::http::Http1ClientConnection> transient;
    fiber::http::Http1ClientConnection *conn = nullptr;
};

// Unified acquire + connect path. Resolves the peer identity to a connected Http1ClientConnection:
//   1. With Pooled policy, pool.acquire(key) -> hit (has_connection) => reuse, zero DNS.
//   2. miss => resolve the dial target(s): IP-key peers use the key's ip directly; name-key peers
//      resolve via DnsService on the calling worker loop (all A/AAAA records, V6First), then dial
//      each address in turn, falling back to the next on connect failure.
//   3. emplace_connection(opts) + connect() (pooled), or construct a transient connection +
//      connect() when no pool is configured or Transient was requested. On a pooled miss the
//      failed lease is reset (park_entry recycles the non-reusable connection) and a fresh slot
//      is acquired per retry.
// `tls_server_name` is forwarded to TlsOptions.server_name for HTTPS keys (SNI); ignored for HTTP.
[[nodiscard]] fiber::async::Task<fiber::common::IoResult<AcquiredUpstreamConnection>>
acquire_and_connect(ConnectionPool &pool, fiber::lite_nginx::runtime::DnsService &dns,
                    const fiber::http::Http1ConnectionGroupKey &key, std::string_view tls_server_name,
                    std::chrono::milliseconds connect_timeout,
                    ConnectionReusePolicy reuse_policy = ConnectionReusePolicy::Pooled) noexcept;

} // namespace fiber::lite_nginx::upstream

#endif // FIBER_LITE_NGINX_UPSTREAM_UPSTREAM_CONNECTION_H
