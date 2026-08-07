#ifndef FIBER_ACCESS_SERVER_PROXY_UPSTREAM_CONNECTION_H
#define FIBER_ACCESS_SERVER_PROXY_UPSTREAM_CONNECTION_H

#include <fiber/async/Task.h>
#include <fiber/common/IoError.h>
#include <fiber/http/StealableHttp1ConnectionPoolSet.h>
#include <fiber/net/IpAddress.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <string_view>
#include <vector>

namespace fiber::http {
class Http1ClientConnection;
}

namespace fiber::access_server {

struct ProxyDnsResolver {
    using Function = async::Task<common::IoResult<std::vector<net::IpAddress>>> (*)(void *context,
                                                                                    std::string_view host) noexcept;

    void *context = nullptr;
    Function resolve = nullptr;
};

enum class ProxyConnectErrorCode : std::uint8_t {
    Resolve,
    PoolShutdown,
    Connect,
};

struct ProxyConnectError {
    ProxyConnectErrorCode code = ProxyConnectErrorCode::Connect;
    common::IoErr io_error = common::IoErr::None;
    const char *message = nullptr;
};

struct ProxyUpstreamConnection {
    http::StealableHttp1ConnectionPoolSet::Lease lease;
    http::Http1ClientConnection *connection = nullptr;
};

[[nodiscard]] async::Task<std::expected<ProxyUpstreamConnection, ProxyConnectError>>
acquire_proxy_upstream_connection(http::StealableHttp1ConnectionPoolSet &pool, ProxyDnsResolver dns_resolver,
                                  const http::Http1ConnectionGroupKey &key,
                                  std::chrono::milliseconds connect_timeout) noexcept;

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_PROXY_UPSTREAM_CONNECTION_H
