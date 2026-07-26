#include "LlmRequestAuthenticator.h"

#include "../config/LlmConfigSnapshot.h"

#include <utility>

#include <http/HttpHeaderHash.h>
#include <http/HttpHeaders.h>

namespace fiber::ai_server {
namespace {

constexpr std::string_view kAuthorization = "authorization";
constexpr std::string_view kApiKey = "x-api-key";
constexpr std::string_view kBearerPrefix = "Bearer ";
constexpr std::string_view kBearerScheme = "bearer";
constexpr std::uint64_t kAuthorizationHash = http::http_header_name_hash(kAuthorization);
constexpr std::uint64_t kApiKeyHash = http::http_header_name_hash(kApiKey);

Bt1AuthError invalid(Bt1AuthFailureReason reason) noexcept {
    return {
            .kind = Bt1AuthErrorKind::InvalidToken,
            .reason = reason,
    };
}

Bt1AuthError internal(Bt1AuthFailureReason reason) noexcept {
    return {
            .kind = Bt1AuthErrorKind::InternalError,
            .reason = reason,
    };
}

bool starts_with_bearer(std::string_view header) noexcept {
    if (header.size() < kBearerPrefix.size() || header[kBearerPrefix.size() - 1] != ' ') {
        return false;
    }
    for (std::size_t i = 0; i < kBearerScheme.size(); ++i) {
        unsigned char ch = static_cast<unsigned char>(header[i]);
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<unsigned char>(ch - 'A' + 'a');
        }
        if (ch != static_cast<unsigned char>(kBearerScheme[i])) {
            return false;
        }
    }
    return true;
}

std::string_view trim_java(std::string_view value) noexcept {
    while (!value.empty() && static_cast<unsigned char>(value.front()) <= 0x20) {
        value.remove_prefix(1);
    }
    while (!value.empty() && static_cast<unsigned char>(value.back()) <= 0x20) {
        value.remove_suffix(1);
    }
    return value;
}

} // namespace

AuthenticatedLlmRequest::AuthenticatedLlmRequest(std::shared_ptr<const LlmConfigSnapshot> config,
                                                 Bt1Principal principal) noexcept :
    config_(std::move(config)), principal_(std::move(principal)) {}

std::expected<std::string_view, Bt1AuthError> extract_llm_request_token(const http::HttpHeaders &headers) noexcept {
    const std::string_view authorization = headers.get(kAuthorization, kAuthorizationHash);
    if (!authorization.empty()) {
        if (!starts_with_bearer(authorization)) {
            return std::unexpected(invalid(Bt1AuthFailureReason::InvalidAuthorizationScheme));
        }
        const std::string_view token = trim_java(authorization.substr(kBearerPrefix.size()));
        if (token.empty()) {
            return std::unexpected(invalid(Bt1AuthFailureReason::MissingCredential));
        }
        return token;
    }

    const std::string_view api_key = headers.get(kApiKey, kApiKeyHash);
    if (api_key.empty()) {
        return std::unexpected(invalid(Bt1AuthFailureReason::MissingCredential));
    }
    return api_key;
}

std::expected<AuthenticatedLlmRequest, Bt1AuthError>
authenticate_llm_request(const http::HttpHeaders &headers, std::shared_ptr<const LlmConfigSnapshot> config,
                         std::int64_t now_seconds) noexcept {
    if (!config || !config->bt1_keys) {
        return std::unexpected(internal(Bt1AuthFailureReason::AuthConfigUnavailable));
    }
    auto token = extract_llm_request_token(headers);
    if (!token) {
        return std::unexpected(token.error());
    }
    auto principal = verify_bt1_token(*token, *config->bt1_keys, now_seconds);
    if (!principal) {
        return std::unexpected(principal.error());
    }
    return AuthenticatedLlmRequest(std::move(config), std::move(*principal));
}

} // namespace fiber::ai_server
