#ifndef FIBER_ACCESS_SERVER_PROXY_EXECUTOR_H
#define FIBER_ACCESS_SERVER_PROXY_EXECUTOR_H

#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include "../routing/ProxyAddressSelector.h"
#include "AccessRequestHandler.h"
#include "ProxyUpstreamConnection.h"

#include <chrono>
#include <cstddef>

namespace fiber::access_server {

struct ProxyExecutorOptions {
    std::chrono::milliseconds connect_timeout{3000};
    std::size_t request_body_chunk_size = 64 * 1024;
    std::size_t response_body_chunk_size = 64 * 1024;
    std::chrono::milliseconds downstream_write_timeout{30000};
};

class ProxyExecutor final : public common::NonCopyable, public common::NonMovable {
public:
    explicit ProxyExecutor(http::StealableHttp1ConnectionPoolSet &pool, ProxyClusterMatcher cluster_matcher = {},
                           ProxyDnsResolver dns_resolver = {}, ProxyExecutorOptions options = {}) noexcept;

    [[nodiscard]] AccessProxyAdapter adapter() noexcept;
    [[nodiscard]] async::Task<Result<void>> execute(http::HttpExchange &exchange, const CompiledProxyRoute &proxy,
                                                    ProxyExecutionInput input,
                                                    AccessRequestTelemetry &telemetry) noexcept;

private:
    static async::Task<Result<void>> execute_adapter(void *context, http::HttpExchange &exchange,
                                                     const CompiledProxyRoute &proxy, ProxyExecutionInput input,
                                                     AccessRequestTelemetry &telemetry) noexcept;
    [[nodiscard]] async::Task<Result<void>> execute_impl(http::HttpExchange &exchange, const CompiledProxyRoute &proxy,
                                                         ProxyExecutionInput input,
                                                         AccessRequestTelemetry &telemetry) noexcept;

    http::StealableHttp1ConnectionPoolSet &pool_;
    ProxyClusterMatcher cluster_matcher_{};
    ProxyDnsResolver dns_resolver_{};
    ProxyExecutorOptions options_{};
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_PROXY_EXECUTOR_H
