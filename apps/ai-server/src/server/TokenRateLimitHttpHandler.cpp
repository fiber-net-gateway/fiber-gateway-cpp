#include "TokenRateLimitHttpHandler.h"

#include "../limit/RateLimitHttpCodec.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <string>
#include <string_view>

#include <common/IoError.h>
#include <common/mem/IoBuf.h>
#include <event/EventLoop.h>
#include <http/HttpBodySpec.h>
#include <http/HttpExchange.h>
#include <http/HttpExchangeIo.h>
#include <http/HttpHeaders.h>

namespace fiber::ai_server {
namespace {

enum class BodyReadError : std::uint8_t {
    Read,
    TooLarge,
    OutOfMemory,
};

std::int64_t wall_now_millis() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
}

bool ascii_equal_ci(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        unsigned char l = static_cast<unsigned char>(left[i]);
        unsigned char r = static_cast<unsigned char>(right[i]);
        if (l >= 'A' && l <= 'Z') {
            l = static_cast<unsigned char>(l - 'A' + 'a');
        }
        if (r >= 'A' && r <= 'Z') {
            r = static_cast<unsigned char>(r - 'A' + 'a');
        }
        if (l != r) {
            return false;
        }
    }
    return true;
}

std::string_view trim_ascii(std::string_view value) noexcept {
    while (!value.empty() &&
           (value.front() == ' ' || value.front() == '\t' || value.front() == '\r' || value.front() == '\n')) {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
           (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' || value.back() == '\n')) {
        value.remove_suffix(1);
    }
    return value;
}

bool is_json_content_type(std::string_view value) noexcept {
    const std::size_t semicolon = value.find(';');
    if (semicolon != std::string_view::npos) {
        value = value.substr(0, semicolon);
    }
    return ascii_equal_ci(trim_ascii(value), "application/json");
}

bool ensure_capacity(mem::IoBuf &buffer, std::size_t required) noexcept {
    if (buffer && buffer.capacity() >= required) {
        return true;
    }
    std::size_t capacity = std::max<std::size_t>(buffer.capacity(), 1024);
    while (capacity < required) {
        const std::size_t next = capacity > kMaxRateLimitHttpBodyBytes / 2 ? kMaxRateLimitHttpBodyBytes : capacity * 2;
        if (next <= capacity) {
            return false;
        }
        capacity = next;
    }
    mem::IoBuf replacement = mem::IoBuf::allocate(capacity);
    if (!replacement) {
        return false;
    }
    if (buffer && buffer.readable() != 0) {
        std::memcpy(replacement.writable_data(), buffer.readable_data(), buffer.readable());
        replacement.commit(buffer.readable());
    }
    buffer = std::move(replacement);
    return true;
}

async::Task<std::expected<mem::IoBuf, BodyReadError>> read_body(http::HttpExchange &exchange) noexcept {
    const http::HttpBodySpec spec = exchange.request_body_spec();
    if (spec.is_content_length() && spec.content_length() > kMaxRateLimitHttpBodyBytes) {
        co_return std::unexpected(BodyReadError::TooLarge);
    }
    mem::IoBuf body;
    if (spec.is_content_length()) {
        body = mem::IoBuf::allocate(std::max<std::size_t>(spec.content_length(), 1));
        if (!body) {
            co_return std::unexpected(BodyReadError::OutOfMemory);
        }
    }
    for (;;) {
        const std::size_t current = body ? body.readable() : 0;
        const std::size_t remaining = kMaxRateLimitHttpBodyBytes - current;
        auto chunk = co_await exchange.read_body(std::min<std::size_t>(remaining + 1, 16 * 1024));
        if (!chunk) {
            co_return std::unexpected(chunk.error() == common::IoErr::MessageTooLarge ? BodyReadError::TooLarge
                                                                                      : BodyReadError::Read);
        }
        if (chunk->readable_bytes() > remaining) {
            co_return std::unexpected(BodyReadError::TooLarge);
        }
        const std::size_t required = current + chunk->readable_bytes();
        if (!ensure_capacity(body, std::max<std::size_t>(required, 1))) {
            co_return std::unexpected(BodyReadError::OutOfMemory);
        }
        while (const mem::IoBuf *part = chunk->first_readable()) {
            const std::size_t size = part->readable();
            std::memcpy(body.writable_data(), part->readable_data(), size);
            body.commit(size);
            chunk->consume_and_compact(size);
        }
        if (chunk->complete()) {
            break;
        }
    }
    co_return std::move(body);
}

async::Task<void> send_json(http::HttpExchange &exchange, int status, std::string_view body,
                            bool allow_post = false) noexcept {
    http::HttpHeaders headers(exchange.pool());
    headers.set_view("Content-Type", "application/json");
    if (allow_post) {
        headers.set_view("Allow", "POST");
    }
    auto sent = co_await exchange.send_header({
            .kind = http::OutgoingHeaderKind::Final,
            .status_code = status,
            .headers = &headers,
            .body = http::HttpBodySpec::ContentLength(body.size()),
            .connection_mode = http::ResponseConnectionMode::Auto,
            .end_stream = body.empty(),
    });
    if (!sent || body.empty()) {
        co_return;
    }
    (void) co_await exchange.write_body(reinterpret_cast<const std::uint8_t *>(body.data()), body.size(), true);
}

async::Task<bool> prepare_request(http::HttpExchange &exchange,
                                  std::expected<mem::IoBuf, BodyReadError> &body) noexcept {
    if (exchange.method() != http::HttpMethod::Post) {
        co_await send_json(exchange, 405, R"({"ok":false,"message":"method not allowed"})", true);
        co_return false;
    }
    const auto *content_type = exchange.content_type_header();
    if (!content_type || !is_json_content_type(content_type->value_view())) {
        co_await send_json(exchange, 415, R"({"ok":false,"message":"content-type must be application/json"})");
        co_return false;
    }
    body = co_await read_body(exchange);
    if (body) {
        co_return true;
    }
    if (body.error() == BodyReadError::TooLarge) {
        co_await send_json(exchange, 413, R"({"ok":false,"message":"request body is too large"})");
    } else if (body.error() == BodyReadError::OutOfMemory) {
        co_await send_json(exchange, 500, R"({"ok":false,"message":"process request failed"})");
    } else {
        co_await send_json(exchange, 400, R"({"ok":false,"message":"invalid json request body"})");
    }
    co_return false;
}

std::string_view body_view(const mem::IoBuf &body) noexcept {
    return body ? std::string_view(reinterpret_cast<const char *>(body.readable_data()), body.readable())
                : std::string_view{};
}

async::Task<void> send_payload_error(http::HttpExchange &exchange, const RateLimitPayloadError &error) noexcept {
    if (error.code == RateLimitPayloadErrorCode::TooLarge) {
        co_await send_json(exchange, 413, R"({"ok":false,"message":"request body is too large"})");
        co_return;
    }
    if (error.code == RateLimitPayloadErrorCode::InvalidValue) {
        co_await send_json(exchange, 400, R"({"ok":false,"message":"invalid rate limit request"})");
        co_return;
    }
    co_await send_json(exchange, 400, R"({"ok":false,"message":"invalid json request body"})");
}

} // namespace

async::Task<void> TokenRateLimitHttpHandler::handle_check(http::HttpExchange &exchange) noexcept {
    std::expected<mem::IoBuf, BodyReadError> body = std::unexpected(BodyReadError::Read);
    if (!co_await prepare_request(exchange, body)) {
        co_return;
    }
    auto request = decode_rate_limit_check_request(body_view(*body), exchange.pool());
    if (!request) {
        co_await send_payload_error(exchange, request.error());
        co_return;
    }
    const TokenRateLimitCheckResult result =
            service_->check(request->user_id, request->model_name,
                            CompiledModelRateLimitRule{
                                    .revision = request->rule_revision,
                                    .window_duration_millis = request->window_duration_millis,
                                    .max_tokens_per_window = request->max_tokens_per_window,
                            },
                            wall_now_millis());
    auto encoded = encode_rate_limit_check_response(to_http_response(result));
    if (!encoded) {
        co_await send_json(exchange, 500, R"({"ok":false,"message":"process request failed"})");
        co_return;
    }
    co_await send_json(exchange, 200, *encoded);
}

async::Task<void> TokenRateLimitHttpHandler::handle_settle(http::HttpExchange &exchange) noexcept {
    std::expected<mem::IoBuf, BodyReadError> body = std::unexpected(BodyReadError::Read);
    if (!co_await prepare_request(exchange, body)) {
        co_return;
    }
    auto request = decode_rate_limit_settle_request(body_view(*body), exchange.pool());
    if (!request) {
        co_await send_payload_error(exchange, request.error());
        co_return;
    }
    const TokenRateLimitSettleResult result =
            service_->settle(request->user_id, request->model_name,
                             TokenRateLimitTicket{
                                     .rule_revision = request->ticket->rule_revision,
                                     .generation = request->ticket->generation,
                                     .window_start_millis = request->ticket->window_start_millis,
                             },
                             request->tokens, request->count_usage, wall_now_millis());
    auto encoded = encode_rate_limit_settle_response(to_http_response(result));
    if (!encoded) {
        co_await send_json(exchange, 500, R"({"ok":false,"message":"process request failed"})");
        co_return;
    }
    co_await send_json(exchange, 200, *encoded);
}

} // namespace fiber::ai_server
