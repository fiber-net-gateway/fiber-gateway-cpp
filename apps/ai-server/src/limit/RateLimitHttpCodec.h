#ifndef FIBER_AI_SERVER_RATE_LIMIT_HTTP_CODEC_H
#define FIBER_AI_SERVER_RATE_LIMIT_HTTP_CODEC_H

#include "TokenRateLimiter.h"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include <common/mem/BufPool.h>

namespace fiber::ai_server {

struct RateLimitCheckRequest {
    std::string_view user_id;
    std::string_view model_name;
    std::int64_t rule_revision = 0;
    std::int64_t window_duration_millis = 0;
    std::int64_t max_tokens_per_window = 0;
};

struct RateLimitTicketPayload {
    std::int64_t rule_revision = 0;
    std::uint64_t generation = 0;
    std::int64_t window_start_millis = 0;
};

struct RateLimitCheckResponse {
    bool rule_matched = false;
    bool allowed = false;
    std::int64_t used_tokens = 0;
    std::int64_t max_tokens = 0;
    std::int64_t recover_at_millis = 0;
    std::optional<RateLimitTicketPayload> ticket;
};

struct RateLimitSettleRequest {
    std::string_view user_id;
    std::string_view model_name;
    std::optional<RateLimitTicketPayload> ticket;
    std::int64_t tokens = 0;
    bool count_usage = false;
};

struct RateLimitSettleResponse {
    bool applied = false;
    bool stale = false;
    bool usage_counted = false;
    std::int64_t used_tokens = 0;
    std::int64_t recover_at_millis = 0;
};

enum class RateLimitPayloadErrorCode : std::uint8_t {
    EmptyBody,
    InvalidJson,
    InvalidValue,
    TooLarge,
    EncodeFailed,
};

struct RateLimitPayloadError {
    RateLimitPayloadErrorCode code = RateLimitPayloadErrorCode::InvalidJson;
    const char *message = nullptr;
};

inline constexpr std::size_t kMaxRateLimitHttpBodyBytes = 64 * 1024;

[[nodiscard]] std::expected<RateLimitCheckRequest, RateLimitPayloadError>
decode_rate_limit_check_request(std::string_view body, mem::BufPool &pool) noexcept;
[[nodiscard]] std::expected<RateLimitCheckResponse, RateLimitPayloadError>
decode_rate_limit_check_response(std::string_view body, mem::BufPool &pool) noexcept;
[[nodiscard]] std::expected<RateLimitSettleRequest, RateLimitPayloadError>
decode_rate_limit_settle_request(std::string_view body, mem::BufPool &pool) noexcept;
[[nodiscard]] std::expected<RateLimitSettleResponse, RateLimitPayloadError>
decode_rate_limit_settle_response(std::string_view body, mem::BufPool &pool) noexcept;

[[nodiscard]] std::expected<std::string, RateLimitPayloadError>
encode_rate_limit_check_request(const RateLimitCheckRequest &value);
[[nodiscard]] std::expected<std::string, RateLimitPayloadError>
encode_rate_limit_check_response(const RateLimitCheckResponse &value);
[[nodiscard]] std::expected<std::string, RateLimitPayloadError>
encode_rate_limit_settle_request(const RateLimitSettleRequest &value);
[[nodiscard]] std::expected<std::string, RateLimitPayloadError>
encode_rate_limit_settle_response(const RateLimitSettleResponse &value);

[[nodiscard]] RateLimitCheckResponse to_http_response(const TokenRateLimitCheckResult &value) noexcept;
[[nodiscard]] RateLimitSettleResponse to_http_response(const TokenRateLimitSettleResult &value) noexcept;

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_RATE_LIMIT_HTTP_CODEC_H
