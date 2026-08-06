#ifndef FIBER_ACCESS_SERVER_ACCESS_REQUEST_HANDLER_H
#define FIBER_ACCESS_SERVER_ACCESS_REQUEST_HANDLER_H

#include "../../../../src/async/Task.h"
#include "../../../../src/common/IoError.h"
#include "../routing/ProjectRouteSnapshot.h"
#include "../runtime/RouteConfigStore.h"
#include "ErrorResponder.h"
#include "ResponseExecutor.h"
#include "TemplateEvaluator.h"

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace fiber::http {
class HttpExchange;
}

namespace fiber::access_server {

class AccessRequestTelemetry;

struct AccessRequestScriptAdapter {
    using ConditionFunction = bool (*)(void *context, http::HttpExchange &exchange,
                                       std::span<const PathVariable> path_variables,
                                       std::string_view request_context_cluster, const void *program,
                                       std::string_view expression) noexcept;
    using TemplateFunction = bool (*)(void *context, http::HttpExchange &exchange,
                                      std::span<const PathVariable> path_variables,
                                      std::string_view request_context_cluster, const void *program,
                                      std::string_view expression, std::string &output, AccessError &error) noexcept;

    void *context = nullptr;
    ConditionFunction evaluate_condition = nullptr;
    TemplateFunction evaluate_template = nullptr;
};

struct AccessRequestHandlerOptions {
    // Matches fiber-net-gateway ServerConfig.DEF_MAX_BODY_SIZE.
    std::size_t default_max_request_body_size = 4U << 20U;
    ResponseExecutorOptions response{};
    bool test_mode = false;
};

struct ProxyExecutionInput {
    std::string_view project;
    std::string_view initial_context_cluster;
    std::string_view origin_host;
    TemplateEvaluator template_evaluator;
    std::size_t max_request_body_size = 0;
};

struct AccessProxyAdapter {
    using ExecuteFunction = async::Task<common::IoResult<void>> (*)(void *context, http::HttpExchange &exchange,
                                                                    const CompiledProxyRoute &proxy,
                                                                    ProxyExecutionInput input,
                                                                    std::span<const EvaluatedHeader> base_headers,
                                                                    AccessRequestTelemetry *telemetry) noexcept;

    void *context = nullptr;
    ExecuteFunction execute = nullptr;
};

class AccessRequestHandler {
public:
    AccessRequestHandler(const RouteConfigStore &config_store, AccessRequestScriptAdapter script_adapter = {},
                         AccessRequestHandlerOptions options = {}, AccessProxyAdapter proxy_adapter = {}) noexcept;

    [[nodiscard]] async::Task<void> handle(http::HttpExchange &exchange,
                                           AccessRequestTelemetry *telemetry = nullptr) const noexcept;

private:
    [[nodiscard]] async::Task<common::IoResult<void>> handle_impl(http::HttpExchange &exchange,
                                                                  AccessRequestTelemetry *telemetry) const noexcept;

    const RouteConfigStore &config_store_;
    AccessRequestScriptAdapter script_adapter_;
    AccessRequestHandlerOptions options_;
    AccessProxyAdapter proxy_adapter_;
    std::array<EvaluatedHeader, 1> project_base_headers_;
    ResponseExecutor response_executor_;
    ErrorResponder error_responder_;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_REQUEST_HANDLER_H
