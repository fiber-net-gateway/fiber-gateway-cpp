#ifndef FIBER_ACCESS_SERVER_PROXY_REQUEST_PLAN_H
#define FIBER_ACCESS_SERVER_PROXY_REQUEST_PLAN_H

#include "../../../../src/http/HttpBodySpec.h"
#include "../../../../src/http/HttpCommon.h"
#include "../routing/ProjectRouteSnapshot.h"
#include "AccessError.h"
#include "ResponsePlan.h"
#include "TemplateEvaluator.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fiber::http {
class HttpExchange;
}

namespace fiber::access_server {

struct PreparedProxyRequest {
    explicit PreparedProxyRequest(const CompiledHeaderTemplates &configured_response_headers) noexcept :
        response_headers(configured_response_headers) {}

    // Views into the pinned route snapshot. The plan must not outlive the
    // AccessRequestHandler invocation that created it.
    ProxyUpstreamKind upstream_kind = ProxyUpstreamKind::Service;
    std::string_view service;
    std::optional<std::string_view> cluster;
    // Evaluated HI-TRACE-CLUSTER/cluster context overrides the route default
    // for service discovery. Empty context values remove the override.
    std::optional<std::string> context_cluster;
    std::span<const CompiledProxyAddress> addresses;

    http::HttpMethod method = http::HttpMethod::Unknown;
    std::string request_target;
    // Does not contain the selected upstream's default Host. A configured Host
    // override is present and must replace that default.
    std::vector<EvaluatedHeader> headers;
    // Empty values mean remove the trace user-data key, matching Java.
    std::vector<EvaluatedHeader> context;
    std::reference_wrapper<const CompiledHeaderTemplates> response_headers;
    // The callback context is owned by AccessRequestHandler and is valid for
    // the synchronous lifetime of the proxy adapter invocation.
    TemplateEvaluator response_evaluator;
    http::HttpBodySpec body = http::HttpBodySpec::None();
    std::size_t max_request_body_size = 0;
    std::int32_t timeout_millis = 60000;
    std::optional<std::uint64_t> max_response_body_size;
    std::int32_t websocket_timeout_millis = 0;
    bool websocket_upgrade = false;
    bool flush = false;
};

using PreparedProxyRequestResult = std::expected<PreparedProxyRequest, AccessError>;

[[nodiscard]] bool is_java_filtered_proxy_request_header(std::string_view name) noexcept;
[[nodiscard]] std::string java_escape_uri(std::string_view value);
[[nodiscard]] PreparedProxyRequestResult
prepare_proxy_request(const http::HttpExchange &exchange, std::string_view project, const CompiledProxyRoute &proxy,
                      TemplateEvaluator evaluator, std::size_t max_request_body_size,
                      std::string_view initial_context_cluster = {});

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_PROXY_REQUEST_PLAN_H
