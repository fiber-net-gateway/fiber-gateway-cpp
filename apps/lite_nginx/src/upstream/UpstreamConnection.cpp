#include "UpstreamConnection.h"

#include <array>
#include <span>
#include <utility>
#include <vector>

#include <fiber/event/EventLoop.h>

#include "../runtime/DnsService.h"

namespace fiber::lite_nginx::upstream {

fiber::async::Task<fiber::common::IoResult<AcquiredUpstreamConnection>>
acquire_and_connect(ConnectionPool &pool, fiber::lite_nginx::runtime::DnsService &dns,
                    const fiber::http::Http1ConnectionGroupKey &key, std::string_view tls_server_name,
                    std::chrono::milliseconds connect_timeout, ConnectionReusePolicy reuse_policy) noexcept {
    AcquiredUpstreamConnection out;
    if (reuse_policy == ConnectionReusePolicy::Pooled) {
        out.lease = co_await pool.acquire(key);
    }

    // Pool hit: reuse the idle connection directly (zero DNS, zero dial).
    if (out.lease.valid() && out.lease.has_connection()) {
        out.conn = out.lease.get();
        co_return out;
    }

    // Resolve the dial target(s). DNS remains separate from the bounded TCP connection race.
    std::vector<fiber::net::IpAddress> addresses;
    if (key.is_ip()) {
        addresses.push_back(key.ip_address());
    } else {
        auto resolved = co_await dns.resolve(key.host_name());
        if (!resolved) {
            co_return std::unexpected(resolved.error());
        }
        addresses = std::move(*resolved);
    }
    if (addresses.empty()) {
        co_return std::unexpected(fiber::common::IoErr::NotFound);
    }
    if (addresses.size() > fiber::net::kHappyEyeballsMaxAddresses) {
        co_return std::unexpected(fiber::common::IoErr::MessageTooLarge);
    }

    std::array<fiber::net::SocketAddress, fiber::net::kHappyEyeballsMaxAddresses> peers{};
    for (std::size_t i = 0; i < addresses.size(); ++i) {
        peers[i] = fiber::net::SocketAddress(addresses[i], key.port());
    }
    const std::span<const fiber::net::SocketAddress> peer_span(peers.data(), addresses.size());

    fiber::http::Http1ClientConnectionOptions connection_options;
    connection_options.peer_addr = peers[0];
    connection_options.pool_affinity = key.pool_affinity();
    if (key.scheme() == fiber::http::Http1ConnectionGroupKey::Scheme::Https) {
        connection_options.tls.emplace();
        connection_options.tls->server_name = std::string(tls_server_name);
    }

    fiber::net::HappyEyeballsOptions connect_options;
    connect_options.total_timeout = connect_timeout;

    if (!out.lease.valid()) {
        out.transient = std::make_unique<fiber::http::Http1ClientConnection>(fiber::event::EventLoop::current(),
                                                                             std::move(connection_options));
        auto connect_result = co_await out.transient->connect(peer_span, connect_options);
        if (!connect_result) {
            co_return std::unexpected(connect_result.error());
        }
        out.conn = out.transient.get();
        co_return out;
    }

    // A pool miss keeps one lease through the TCP race and optional TLS handshake. The connection
    // is exposed only after the full connect path succeeds; reset recycles a failed, non-reusable
    // entry.
    auto emplace = out.lease.emplace_connection(std::move(connection_options));
    if (!emplace) {
        fiber::common::IoErr error = emplace.error();
        out.lease.reset();
        co_return std::unexpected(error);
    }
    auto connect_result = co_await (*emplace)->connect(peer_span, connect_options);
    if (!connect_result) {
        fiber::common::IoErr error = connect_result.error();
        out.lease.reset();
        co_return std::unexpected(error);
    }
    out.conn = *emplace;
    co_return out;
}

} // namespace fiber::lite_nginx::upstream
