#include "ProxyUpstreamConnection.h"

#include <fiber/http/Http1ClientConnection.h>
#include <fiber/http/Http1ConnectionGroupKey.h>
#include <fiber/net/SocketAddress.h>

#include <span>
#include <utility>

namespace fiber::access_server {
namespace {

ProxyConnectError error(ProxyConnectErrorCode code, const char *message,
                        common::IoErr io_error = common::IoErr::None) noexcept {
    return ProxyConnectError{
            .code = code,
            .io_error = io_error,
            .message = message,
    };
}

http::Http1ClientConnectionOptions connection_options(const http::Http1ConnectionGroupKey &key,
                                                      const net::IpAddress &ip) {
    http::Http1ClientConnectionOptions result;
    result.peer_addr = net::SocketAddress(ip, key.port());
    if (key.scheme() == http::Http1ConnectionGroupKey::Scheme::Https) {
        result.tls.enabled = true;
        // The Java client uses InsecureTrustManagerFactory by default.
        result.tls.verify_peer = false;
        if (key.is_name()) {
            result.tls.server_name.assign(key.host_name());
        }
    }
    return result;
}

} // namespace

async::Task<std::expected<ProxyUpstreamConnection, ProxyConnectError>>
acquire_proxy_upstream_connection(http::StealableHttp1ConnectionPoolSet &pool, ProxyDnsResolver dns_resolver,
                                  const http::Http1ConnectionGroupKey &key,
                                  std::chrono::milliseconds connect_timeout) noexcept {
    ProxyUpstreamConnection output;
    output.lease = co_await pool.acquire(key);
    if (!output.lease.valid()) {
        co_return std::unexpected(error(ProxyConnectErrorCode::PoolShutdown,
                                        "upstream connection pool is shutting down", common::IoErr::Canceled));
    }
    if (output.lease.has_connection()) {
        output.connection = output.lease.get();
        co_return std::move(output);
    }

    std::vector<net::IpAddress> resolved;
    std::span<const net::IpAddress> addresses;
    if (key.is_ip()) {
        addresses = std::span(&key.ip_address(), 1);
    } else {
        if (!dns_resolver.resolve) {
            co_return std::unexpected(error(ProxyConnectErrorCode::Resolve, "upstream DNS resolver is unavailable",
                                            common::IoErr::NotFound));
        }
        auto result = co_await dns_resolver.resolve(dns_resolver.context, key.host_name());
        if (!result) {
            co_return std::unexpected(
                    error(ProxyConnectErrorCode::Resolve, "upstream DNS resolution failed", result.error()));
        }
        resolved = std::move(*result);
        addresses = resolved;
    }
    if (addresses.empty()) {
        co_return std::unexpected(
                error(ProxyConnectErrorCode::Resolve, "upstream DNS returned no address", common::IoErr::NotFound));
    }

    common::IoErr last_error = common::IoErr::NotFound;
    for (std::size_t i = 0; i < addresses.size(); ++i) {
        if (i > 0) {
            output.lease = co_await pool.acquire(key);
            if (!output.lease.valid()) {
                co_return std::unexpected(error(ProxyConnectErrorCode::PoolShutdown,
                                                "upstream connection pool is shutting down", common::IoErr::Canceled));
            }
            if (output.lease.has_connection()) {
                output.connection = output.lease.get();
                co_return std::move(output);
            }
        }

        auto emplaced = output.lease.emplace_connection(connection_options(key, addresses[i]));
        if (!emplaced) {
            last_error = emplaced.error();
            output.lease.reset();
            continue;
        }
        auto connected = co_await (*emplaced)->connect(connect_timeout);
        if (connected) {
            output.connection = *emplaced;
            co_return std::move(output);
        }
        last_error = connected.error();
        output.lease.reset();
    }
    co_return std::unexpected(error(ProxyConnectErrorCode::Connect, "upstream connection failed", last_error));
}

} // namespace fiber::access_server
