#ifndef FIBER_ACCESS_SERVER_ERROR_RESPONDER_H
#define FIBER_ACCESS_SERVER_ERROR_RESPONDER_H

#include "../../../../src/async/Task.h"
#include "../../../../src/common/IoError.h"
#include "AccessError.h"
#include "ResponsePlan.h"

#include <chrono>
#include <span>
#include <string>
#include <string_view>

namespace fiber::http {
class HttpExchange;
}

namespace fiber::access_server {

class AccessRequestTelemetry;

struct RenderedError {
    int status = 500;
    std::string content_type;
    std::string body;
};

struct ErrorResponderOptions {
    std::chrono::milliseconds body_timeout{60000};
    std::chrono::milliseconds write_timeout{30000};
};

class ErrorResponder {
public:
    explicit ErrorResponder(ErrorResponderOptions options = {}) noexcept : options_(options) {}

    [[nodiscard]] static bool wants_html(std::string_view accept) noexcept;
    [[nodiscard]] static RenderedError render(const AccessError &error, std::string_view accept,
                                              std::string_view trace_id = "unknown-trace-id");

    [[nodiscard]] async::Task<common::IoResult<void>>
    send(http::HttpExchange &exchange, const AccessError &error, std::span<const EvaluatedHeader> base_headers = {},
         std::span<const EvaluatedHeader> inherited_headers = {}, std::string_view trace_id = "unknown-trace-id",
         bool request_body_discarded = false, AccessRequestTelemetry *telemetry = nullptr) const noexcept;

private:
    ErrorResponderOptions options_;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ERROR_RESPONDER_H
