#ifndef FIBER_AI_SERVER_LLM_REQUEST_AUTHENTICATOR_H
#define FIBER_AI_SERVER_LLM_REQUEST_AUTHENTICATOR_H

#include "Bt1TokenVerifier.h"

#include <cstdint>
#include <expected>
#include <memory>
#include <string_view>

namespace fiber::http {
class HttpHeaders;
}

namespace fiber::ai_server {

struct LlmConfigSnapshot;

class AuthenticatedLlmRequest {
public:
    [[nodiscard]] const LlmConfigSnapshot &config() const noexcept { return *config_; }
    [[nodiscard]] const std::shared_ptr<const LlmConfigSnapshot> &config_handle() const noexcept { return config_; }
    [[nodiscard]] const Bt1Principal &principal() const noexcept { return principal_; }

private:
    AuthenticatedLlmRequest(std::shared_ptr<const LlmConfigSnapshot> config, Bt1Principal principal) noexcept;

    friend std::expected<AuthenticatedLlmRequest, Bt1AuthError>
    authenticate_llm_request(const http::HttpHeaders &headers, std::shared_ptr<const LlmConfigSnapshot> config,
                             std::int64_t now_seconds) noexcept;

    std::shared_ptr<const LlmConfigSnapshot> config_;
    Bt1Principal principal_;
};

[[nodiscard]] std::expected<std::string_view, Bt1AuthError>
extract_llm_request_token(const http::HttpHeaders &headers) noexcept;

[[nodiscard]] std::expected<AuthenticatedLlmRequest, Bt1AuthError>
authenticate_llm_request(const http::HttpHeaders &headers, std::shared_ptr<const LlmConfigSnapshot> config,
                         std::int64_t now_seconds) noexcept;

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_LLM_REQUEST_AUTHENTICATOR_H
