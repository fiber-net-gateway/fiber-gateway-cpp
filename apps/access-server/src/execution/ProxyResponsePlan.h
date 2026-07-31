#ifndef FIBER_ACCESS_SERVER_PROXY_RESPONSE_PLAN_H
#define FIBER_ACCESS_SERVER_PROXY_RESPONSE_PLAN_H

#include "../routing/ProjectRouteSnapshot.h"
#include "AccessError.h"
#include "ResponsePlan.h"
#include "TemplateEvaluator.h"

#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fiber::access_server {

using PreparedProxyResponseHeaders = std::vector<EvaluatedHeader>;
using PreparedProxyResponseHeadersResult = std::expected<PreparedProxyResponseHeaders, AccessError>;

[[nodiscard]] PreparedProxyResponseHeadersResult
prepare_proxy_response_headers(std::span<const CompiledTemplateEntry> headers, TemplateEvaluator evaluator);

[[nodiscard]] bool proxy_response_header_is_configured(std::span<const CompiledTemplateEntry> headers,
                                                       std::string_view name) noexcept;

[[nodiscard]] std::optional<std::string> rewrite_java_proxy_location(std::string_view upstream_value,
                                                                     std::string_view upstream_host,
                                                                     std::string_view downstream_scheme,
                                                                     std::string_view downstream_host);

[[nodiscard]] std::optional<std::string> rewrite_java_proxy_refresh(std::string_view upstream_value,
                                                                    std::string_view upstream_host,
                                                                    std::string_view downstream_scheme,
                                                                    std::string_view downstream_host);

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_PROXY_RESPONSE_PLAN_H
