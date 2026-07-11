#include "UpstreamConnection.h"

#include <utility>

#include "event/EventLoop.h"

#include "../runtime/DnsService.h"

namespace fiber::lite_nginx::upstream {

fiber::async::Task<fiber::common::IoResult<AcquiredUpstreamConnection>>
acquire_and_connect(ConnectionPool &pool, fiber::lite_nginx::runtime::DnsService &dns,
                    const fiber::http::Http1ConnectionGroupKey &key, std::string_view tls_server_name,
                    std::chrono::milliseconds connect_timeout) noexcept {
    AcquiredUpstreamConnection out;
    out.lease = co_await pool.acquire(key);
    if (!out.lease.valid()) {
        // No pool configured (keepalive_size == 0): open a transient connection.
        fiber::net::SocketAddress peer_addr;
        if (key.is_ip()) {
            peer_addr = fiber::net::SocketAddress(key.ip_address(), key.port());
        } else {
            auto resolved = co_await dns.resolve(key.host_name());
            if (!resolved) {
                co_return std::unexpected(resolved.error());
            }
            peer_addr = fiber::net::SocketAddress(*resolved, key.port());
        }
        fiber::http::Http1ClientConnectionOptions opts;
        opts.peer_addr = peer_addr;
        opts.connect_timeout = connect_timeout;
        if (key.scheme() == fiber::http::Http1ConnectionGroupKey::Scheme::Https) {
            opts.tls.server_name = std::string(tls_server_name);
        }
        out.transient = std::make_unique<fiber::http::Http1ClientConnection>(fiber::event::EventLoop::current(), opts);
        auto connect_result = co_await out.transient->connect();
        if (!connect_result) {
            co_return std::unexpected(connect_result.error());
        }
        out.conn = out.transient.get();
        co_return out;
    }

    // Pooled lease. Hit -> reuse directly (zero DNS). Miss -> emplace + connect.
    if (out.lease.has_connection()) {
        out.conn = out.lease.get();
        co_return out;
    }

    fiber::net::SocketAddress peer_addr;
    if (key.is_ip()) {
        peer_addr = fiber::net::SocketAddress(key.ip_address(), key.port());
    } else {
        auto resolved = co_await dns.resolve(key.host_name());
        if (!resolved) {
            co_return std::unexpected(resolved.error());
        }
        peer_addr = fiber::net::SocketAddress(*resolved, key.port());
    }

    fiber::http::Http1ClientConnectionOptions opts;
    opts.peer_addr = peer_addr;
    opts.connect_timeout = connect_timeout;
    if (key.scheme() == fiber::http::Http1ConnectionGroupKey::Scheme::Https) {
        opts.tls.server_name = std::string(tls_server_name);
    }

    auto emplace = out.lease.emplace_connection(opts);
    if (!emplace) {
        co_return std::unexpected(emplace.error());
    }
    auto connect_result = co_await (*emplace)->connect();
    if (!connect_result) {
        co_return std::unexpected(connect_result.error());
    }
    out.conn = *emplace;
    co_return out;
}

} // namespace fiber::lite_nginx::upstream
