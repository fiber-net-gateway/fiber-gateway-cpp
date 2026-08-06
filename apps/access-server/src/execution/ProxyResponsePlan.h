#ifndef FIBER_ACCESS_SERVER_PROXY_RESPONSE_PLAN_H
#define FIBER_ACCESS_SERVER_PROXY_RESPONSE_PLAN_H

#include "../routing/ProjectRouteSnapshot.h"
#include "AccessResult.h"
#include "ResponsePlan.h"
#include "TemplateEvaluator.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fiber::access_server {

using PreparedProxyResponseHeaders = std::vector<EvaluatedHeader>;
using PreparedProxyResponseHeadersResult = Result<PreparedProxyResponseHeaders>;

[[nodiscard]] PreparedProxyResponseHeadersResult prepare_proxy_response_headers(const CompiledHeaderTemplates &headers,
                                                                                TemplateEvaluator evaluator);

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
