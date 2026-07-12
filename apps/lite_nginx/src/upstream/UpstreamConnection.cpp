#include "UpstreamConnection.h"

#include <utility>
#include <vector>

#include "event/EventLoop.h"

#include "../runtime/DnsService.h"

namespace fiber::lite_nginx::upstream {

fiber::async::Task<fiber::common::IoResult<AcquiredUpstreamConnection>>
acquire_and_connect(ConnectionPool &pool, fiber::lite_nginx::runtime::DnsService &dns,
                    const fiber::http::Http1ConnectionGroupKey &key, std::string_view tls_server_name,
                    std::chrono::milliseconds connect_timeout) noexcept {
    AcquiredUpstreamConnection out;
    out.lease = co_await pool.acquire(key);

    // Pool hit: reuse the idle connection directly (zero DNS, zero dial).
    if (out.lease.valid() && out.lease.has_connection()) {
        out.conn = out.lease.get();
        co_return out;
    }

    // Resolve the dial target(s). IP-key peers dial the key's IP directly; name-key peers resolve
    // via DnsService, which returns every A/AAAA record (ordered V6First). We dial each address in
    // turn and fall back to the next on connect failure instead of failing the whole request after
    // a single dead address.
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

    auto build_opts = [&](const fiber::net::IpAddress &ip) {
        fiber::http::Http1ClientConnectionOptions opts;
        opts.peer_addr = fiber::net::SocketAddress(ip, key.port());
        opts.connect_timeout = connect_timeout;
        if (key.scheme() == fiber::http::Http1ConnectionGroupKey::Scheme::Https) {
            opts.tls.server_name = std::string(tls_server_name);
        }
        return opts;
    };

    fiber::common::IoErr last_err = fiber::common::IoErr::NotFound;

    if (!out.lease.valid()) {
        // No pool configured (keepalive_size == 0): open a transient connection per attempt.
        for (std::size_t i = 0; i < addresses.size(); ++i) {
            out.transient = std::make_unique<fiber::http::Http1ClientConnection>(fiber::event::EventLoop::current(),
                                                                                 build_opts(addresses[i]));
            auto connect_result = co_await out.transient->connect();
            if (connect_result) {
                out.conn = out.transient.get();
                co_return out;
            }
            last_err = connect_result.error();
            out.transient.reset();
        }
        co_return std::unexpected(last_err);
    }

    // Pooled lease on a miss. Try each address; on failure reset the lease (park_entry recycles the
    // dead, non-reusable connection) and re-acquire a fresh slot for the next attempt.
    for (std::size_t i = 0; i < addresses.size(); ++i) {
        if (i > 0) {
            out.lease = co_await pool.acquire(key);
            if (!out.lease.valid()) {
                co_return std::unexpected(fiber::common::IoErr::Invalid);
            }
            // A concurrent acquire may have populated the group: reuse it instead of redialing.
            if (out.lease.has_connection()) {
                out.conn = out.lease.get();
                co_return out;
            }
        }
        auto emplace = out.lease.emplace_connection(build_opts(addresses[i]));
        if (!emplace) {
            last_err = emplace.error();
            out.lease.reset();
            continue;
        }
        auto connect_result = co_await (*emplace)->connect();
        if (connect_result) {
            out.conn = *emplace;
            co_return out;
        }
        last_err = connect_result.error();
        out.lease.reset();
    }
    co_return std::unexpected(last_err);
}

} // namespace fiber::lite_nginx::upstream
