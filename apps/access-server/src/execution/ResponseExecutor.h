#ifndef FIBER_ACCESS_SERVER_RESPONSE_EXECUTOR_H
#define FIBER_ACCESS_SERVER_RESPONSE_EXECUTOR_H

#include "../../../../src/async/Task.h"
#include "../../../../src/common/IoError.h"
#include "../routing/ProjectRouteSnapshot.h"
#include "ErrorResponder.h"
#include "TemplateEvaluator.h"

#include <chrono>
#include <cstddef>
#include <span>
#include <string_view>

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
    explicit ResponseExecutor(ResponseExecutorOptions options = {}) noexcept :
        options_(options), error_responder_(ErrorResponderOptions{
                                   .body_timeout = options.body_timeout,
                                   .write_timeout = options.write_timeout,
                           }) {}

    [[nodiscard]] async::Task<common::IoResult<void>>
    execute(http::HttpExchange &exchange, const CompiledRoute &route,
            std::span<const EvaluatedHeader> base_headers = {}, TemplateEvaluator evaluator = {},
            std::size_t max_request_body_size = 0, std::string_view trace_id = "unknown-trace-id",
            AccessRequestTelemetry *telemetry = nullptr) const noexcept;

private:
    ResponseExecutorOptions options_;
    ErrorResponder error_responder_;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_RESPONSE_EXECUTOR_H
