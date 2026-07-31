#ifndef FIBER_ACCESS_SERVER_RESPONSE_PLAN_H
#define FIBER_ACCESS_SERVER_RESPONSE_PLAN_H

#include "../routing/ProjectRouteSnapshot.h"
#include "AccessError.h"
#include "TemplateEvaluator.h"

#include <expected>
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

struct ResponsePreparationError {
    AccessError error;
    // Java commits response headers only after every header template
    // succeeds. A later body-template failure retains that complete set.
    std::vector<EvaluatedHeader> inherited_headers;
};

using PreparedResponseResult = std::expected<PreparedResponse, ResponsePreparationError>;

[[nodiscard]] bool is_java_filtered_response_header(std::string_view name) noexcept;
[[nodiscard]] bool is_valid_http_header_name(std::string_view name) noexcept;
[[nodiscard]] bool is_valid_http_header_value(std::string_view value) noexcept;
[[nodiscard]] PreparedResponseResult prepare_response(const CompiledResponseRoute &response,
                                                      TemplateEvaluator evaluator);

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_RESPONSE_PLAN_H
