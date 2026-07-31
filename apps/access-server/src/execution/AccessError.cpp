#include "AccessError.h"

#include <utility>

namespace fiber::access_server {

AccessError AccessError::router_not_found() {
    return AccessError{
            .status = 404,
            .name = "ROUTER_NOT_FOUND",
            .message = "error find router",
    };
}

AccessError AccessError::bad_request() {
    return AccessError{
            .status = 400,
            .name = "BAD_REQUEST",
            .message = "error find router",
    };
}

AccessError AccessError::url_not_matched(std::string_view project) {
    std::string message = "url not matched is project:";
    message.append(project);
    return AccessError{
            .status = 404,
            .name = "URL_NOT_MATCHED",
            .message = std::move(message),
    };
}

AccessError AccessError::entry_error() {
    return AccessError{
            .status = 403,
            .name = "ENTRY_ERROR",
            .message = "entry error",
    };
}

AccessError AccessError::source_ip_not_allowed() {
    return AccessError{
            .status = 403,
            .name = "NOT_ALLOW_IP",
            .message = "source ip is not allowed",
    };
}

AccessError AccessError::request_body_too_large() {
    return AccessError{
            .status = 413,
            .name = "REQ_BODY_TOO_LARGE",
            .message = "request body is too large",
    };
}

AccessError AccessError::template_script(std::string_view detail) {
    std::string message = "error exec for template expression: ";
    message.append(detail);
    return AccessError{
            .status = 500,
            .name = "TEMPLATE_SCRIPT",
            .message = std::move(message),
    };
}

AccessError AccessError::unknown(std::string_view message) {
    return AccessError{
            .status = 500,
            .name = "ACCESS_UNKNOWN_ERROR",
            .message = message.empty() ? "unknown error" : std::string(message),
    };
}

} // namespace fiber::access_server
