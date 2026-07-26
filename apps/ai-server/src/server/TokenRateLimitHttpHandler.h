#ifndef FIBER_AI_SERVER_TOKEN_RATE_LIMIT_HTTP_HANDLER_H
#define FIBER_AI_SERVER_TOKEN_RATE_LIMIT_HTTP_HANDLER_H

#include "../limit/TokenRateLimitService.h"

#include <async/Task.h>

namespace fiber::http {
class HttpExchange;
}

namespace fiber::ai_server {

class TokenRateLimitHttpHandler {
public:
    explicit TokenRateLimitHttpHandler(TokenRateLimitService &service) noexcept : service_(&service) {}

    [[nodiscard]] async::Task<void> handle_check(http::HttpExchange &exchange) noexcept;
    [[nodiscard]] async::Task<void> handle_settle(http::HttpExchange &exchange) noexcept;

private:
    TokenRateLimitService *service_ = nullptr;
};

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_TOKEN_RATE_LIMIT_HTTP_HANDLER_H
