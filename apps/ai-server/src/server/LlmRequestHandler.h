#ifndef FIBER_AI_SERVER_LLM_REQUEST_HANDLER_H
#define FIBER_AI_SERVER_LLM_REQUEST_HANDLER_H

#include "../config/LlmConfigSnapshot.h"
#include "../limit/TokenRateLimitCoordinator.h"
#include "../observability/AiServerMetrics.h"
#include "../protocol/LlmBody.h"
#include "../provider/ProviderHttpClient.h"
#include "../provider/ProviderRuntime.h"

#include <cstddef>
#include <memory>

#include <async/Task.h>

namespace fiber::http {
class HttpExchange;
}

namespace fiber::cat {
class CatClient;
}

namespace fiber::ai_server {

class LlmRequestHandler {
public:
    LlmRequestHandler(ProviderHttpClient &provider_client, ProviderRuntimeRegistry &provider_runtime,
                      TokenRateLimitCoordinator &rate_limiters, AiServerMetrics::Worker &metrics,
                      cat::CatClient *cat_client, std::size_t audit_max_record_bytes) noexcept :
        provider_client_(&provider_client), provider_runtime_(&provider_runtime), rate_limiters_(&rate_limiters),
        metrics_(&metrics), cat_client_(cat_client), audit_max_record_bytes_(audit_max_record_bytes) {}

    [[nodiscard]] async::Task<void> handle(http::HttpExchange &exchange, LlmWireProtocol protocol,
                                           std::shared_ptr<const LlmConfigSnapshot> config) noexcept;

private:
    ProviderHttpClient *provider_client_ = nullptr;
    ProviderRuntimeRegistry *provider_runtime_ = nullptr;
    TokenRateLimitCoordinator *rate_limiters_ = nullptr;
    AiServerMetrics::Worker *metrics_ = nullptr;
    cat::CatClient *cat_client_ = nullptr;
    std::size_t audit_max_record_bytes_ = 0;
};

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_LLM_REQUEST_HANDLER_H
