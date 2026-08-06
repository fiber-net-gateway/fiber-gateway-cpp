#ifndef FIBER_ACCESS_SERVER_RESPONSE_PLAN_H
#define FIBER_ACCESS_SERVER_RESPONSE_PLAN_H

#include "../routing/ProjectRouteSnapshot.h"
#include "AccessResult.h"
#include "TemplateEvaluator.h"

#include <string>
#include <string_view>
#include <vector>

namespace fiber::access_server {

struct EvaluatedHeader {
    std::string name;
    std::string value;
};

struct PreparedResponse {
    int status = 0;
    std::vector<EvaluatedHeader> headers;
    std::string body;
};

using PreparedResponseResult = Result<PreparedResponse>;

[[nodiscard]] bool is_java_filtered_response_header(std::string_view name) noexcept;
[[nodiscard]] bool is_valid_http_header_name(std::string_view name) noexcept;
[[nodiscard]] bool is_valid_http_header_value(std::string_view value) noexcept;
[[nodiscard]] PreparedResponseResult prepare_response(const CompiledResponseRoute &response,
                                                      TemplateEvaluator evaluator);

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_RESPONSE_PLAN_H
