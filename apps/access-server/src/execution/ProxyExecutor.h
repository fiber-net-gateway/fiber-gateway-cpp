#ifndef FIBER_ACCESS_SERVER_PROXY_EXECUTOR_H
#define FIBER_ACCESS_SERVER_PROXY_EXECUTOR_H

#include "../../../../src/common/NonCopyable.h"
#include "../../../../src/common/NonMovable.h"
#include "AccessRequestHandler.h"
#include "ErrorResponder.h"
#include "ProxyRequestSender.h"

#include <chrono>
#include <cstddef>
#include <span>

namespace fiber::access_server {

struct ProxyExecutorOptions {
    std::size_t response_body_chunk_size = 64 * 1024;
    std::chrono::milliseconds downstream_write_timeout{30000};
    ErrorResponderOptions error{};
};

class ProxyExecutor final : public common::NonCopyable, public common::NonMovable {
public:
    explicit ProxyExecutor(ProxyRequestSender &sender, ProxyExecutorOptions options = {}) noexcept;

    [[nodiscard]] AccessProxyAdapter adapter() noexcept;
    [[nodiscard]] async::Task<common::IoResult<void>> execute(http::HttpExchange &exchange,
                                                              const PreparedProxyRequest &request,
                                                              std::span<const EvaluatedHeader> base_headers,
                                                              AccessRequestTelemetry *telemetry = nullptr) noexcept;

private:
    static async::Task<common::IoResult<void>> execute_adapter(void *context, http::HttpExchange &exchange,
                                                               const PreparedProxyRequest &request,
                                                               std::span<const EvaluatedHeader> base_headers,
                                                               AccessRequestTelemetry *telemetry) noexcept;

    [[nodiscard]] async::Task<common::IoResult<void>> execute_monitored(http::HttpExchange &exchange,
                                                                        const PreparedProxyRequest &request,
                                                                        std::span<const EvaluatedHeader> base_headers,
                                                                        AccessRequestTelemetry *telemetry) noexcept;

    ProxyRequestSender *sender_ = nullptr;
    ProxyExecutorOptions options_{};
    ErrorResponder error_responder_;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_PROXY_EXECUTOR_H
