#ifndef FIBER_ACCESS_SERVER_RESPONSE_EXECUTOR_H
#define FIBER_ACCESS_SERVER_RESPONSE_EXECUTOR_H

#include <fiber/async/Task.h>
#include "../routing/ProjectRouteSnapshot.h"
#include "AccessResult.h"
#include "ResponsePlan.h"
#include "TemplateEvaluator.h"

#include <chrono>
#include <cstddef>

namespace fiber::http {
class HttpExchange;
}

namespace fiber::access_server {

class AccessRequestTelemetry;

struct ResponseExecutorOptions {
    std::chrono::milliseconds body_timeout{60000};
    std::chrono::milliseconds write_timeout{30000};
};

class ResponseExecutor {
public:
    explicit ResponseExecutor(ResponseExecutorOptions options = {}) noexcept : options_(options) {}

    [[nodiscard]] async::Task<Result<void>> execute(http::HttpExchange &exchange, const CompiledRoute &route,
                                                    AccessRequestTelemetry &telemetry, TemplateEvaluator evaluator = {},
                                                    std::size_t max_request_body_size = 0) const noexcept;

private:
    ResponseExecutorOptions options_;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_RESPONSE_EXECUTOR_H
