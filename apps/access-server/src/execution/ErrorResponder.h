#ifndef FIBER_ACCESS_SERVER_ERROR_RESPONDER_H
#define FIBER_ACCESS_SERVER_ERROR_RESPONDER_H

#include <fiber/async/Task.h>
#include <fiber/common/IoError.h>
#include "AccessResult.h"

#include <chrono>
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
    std::chrono::milliseconds write_timeout{30000};
};

class ErrorResponder {
public:
    explicit ErrorResponder(ErrorResponderOptions options = {}) noexcept : options_(options) {}

    [[nodiscard]] static bool wants_html(std::string_view accept) noexcept;
    [[nodiscard]] static RenderedError render(const Exception &error, std::string_view accept,
                                              std::string_view trace_id = "unknown-trace-id");

    [[nodiscard]] async::Task<common::IoResult<void>>
    send(http::HttpExchange &exchange, AccessRequestTelemetry &telemetry, const Exception &error) const noexcept;

private:
    ErrorResponderOptions options_;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ERROR_RESPONDER_H
