#ifndef FIBER_AI_SERVER_TOKEN_RATE_LIMIT_HTTP_HANDLER_H
#define FIBER_AI_SERVER_TOKEN_RATE_LIMIT_HTTP_HANDLER_H

#include "../limit/TokenRateLimitService.h"

#include <fiber/async/Task.h>

namespace fiber::http {
class HttpExchange;
}

namespace fiber::ai_server {

class AiServerCatRequest;

class TokenRateLimitHttpHandler {
public:
    explicit TokenRateLimitHttpHandler(TokenRateLimitService &service,
                                       AiServerCatRequest *cat_request = nullptr) noexcept :
        service_(&service), cat_request_(cat_request) {}

    [[nodiscard]] async::Task<void> handle_check(http::HttpExchange &exchange) noexcept;
    [[nodiscard]] async::Task<void> handle_settle(http::HttpExchange &exchange) noexcept;

private:
    TokenRateLimitService *service_ = nullptr;
    AiServerCatRequest *cat_request_ = nullptr;
};

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_TOKEN_RATE_LIMIT_HTTP_HANDLER_H
