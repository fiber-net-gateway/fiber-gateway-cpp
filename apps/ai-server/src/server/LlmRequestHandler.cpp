#include "LlmRequestHandler.h"

#include "../auth/LlmRequestAuthenticator.h"
#include "../protocol/LlmError.h"
#include "../protocol/SseParser.h"
#include "../protocol/TokenUsage.h"
#include "../provider/ExecutionPlan.h"
#include "../provider/ProviderErrorClassifier.h"
#include "../routing/ModelAuthorization.h"
#include "../routing/ProviderRouteKey.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <openssl/sha.h>

#include <common/Assert.h>
#include <common/IoError.h>
#include <event/EventLoop.h>
#include <fiber/cat/CatClient.h>
#include <fiber/cat/MessageTrace.h>
#include <fiber/cat/Status.h>
#include <http/HttpBodySpec.h>
#include <http/HttpExchange.h>
#include <http/HttpExchangeIo.h>
#include <http/HttpHeaders.h>
#include <log/Log.h>

namespace fiber::ai_server {
namespace {

DEFINE_LOGGER(LOG_LLM, "ai_server.llm");
DEFINE_LOGGER(LOG_AUDIT, "ai_server.audit");

constexpr std::size_t kMaxRequestBodyBytes = 4 * 1024 * 1024;
constexpr std::size_t kMaxProviderErrorBytes = 4 * 1024 * 1024;
constexpr std::size_t kMaxProviderResponseBytes = 32 * 1024 * 1024;
constexpr std::size_t kBodyChunkBytes = 64 * 1024;
constexpr std::chrono::seconds kProviderTimeout{300};

std::int64_t wall_now_millis() noexcept;

std::string_view protocol_name(LlmWireProtocol protocol) noexcept {
    return protocol == LlmWireProtocol::OpenAiChatCompletions ? std::string_view("openai")
                                                              : std::string_view("anthropic");
}

template<std::size_t Capacity>
class FixedAuditText {
public:
    void assign(std::string_view value) noexcept {
        size_ = std::min<std::size_t>(Capacity, value.size());
        if (size_ != 0) {
            std::memcpy(value_.data(), value.data(), size_);
        }
    }

    [[nodiscard]] std::string_view view() const noexcept { return {value_.data(), size_}; }

private:
    std::array<char, Capacity> value_{};
    std::size_t size_ = 0;
};

class LlmRequestAudit {
public:
    LlmRequestAudit(http::HttpExchange &exchange, LlmWireProtocol protocol, cat::CatClient *cat_client) noexcept :
        exchange_(&exchange), protocol_(protocol), started_(event::EventLoop::current().now()) {
        static std::atomic<std::uint64_t> sequence{0};
        const std::uint64_t next = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
        const int length = std::snprintf(request_id_storage_.data(), request_id_storage_.size(), "%llx-%llx",
                                         static_cast<unsigned long long>(wall_now_millis()),
                                         static_cast<unsigned long long>(next));
        if (length > 0) {
            request_id_ =
                    std::string_view(request_id_storage_.data(), std::min<std::size_t>(static_cast<std::size_t>(length),
                                                                                       request_id_storage_.size() - 1));
        }
        if (cat_client) {
            auto trace = cat::MessageTrace::create(*cat_client);
            if (trace) {
                cat_trace_.emplace(std::move(*trace));
                auto context = cat_trace_->propagation_context();
                if (context) {
                    const std::string_view message_id = context->message_id();
                    const std::size_t size = std::min<std::size_t>(message_id.size(), request_id_storage_.size() - 1);
                    std::memcpy(request_id_storage_.data(), message_id.data(), size);
                    request_id_ = std::string_view(request_id_storage_.data(), size);
                }
                auto transaction = cat_trace_->create_transaction("URL", exchange.uri().path);
                if (transaction) {
                    cat_transaction_.emplace(std::move(*transaction));
                    (void) cat_transaction_->add_data("protocol", protocol_name(protocol_));
                }
            }
        }
    }

    ~LlmRequestAudit() {
        const http::HttpResponseStats &response = exchange_->response_stats();
        const auto duration =
                std::chrono::duration_cast<std::chrono::microseconds>(event::EventLoop::current().now() - started_);
        LOG(LOG_AUDIT, INFO) << "event=request"
                             << " request_id=" << log::quoted(request_id_) << " protocol=" << protocol_name(protocol_)
                             << " remote_addr=" << log::quoted(exchange_->remote_addr().to_string())
                             << " method=" << static_cast<int>(exchange_->method())
                             << " path=" << log::quoted(exchange_->uri().path) << " auth=" << auth_result_
                             << " auth_reason=" << auth_reason_ << " user=" << log::quoted(user_.view())
                             << " kid=" << log::quoted(kid_.view())
                             << " requested_model=" << log::quoted(requested_model_.view())
                             << " model=" << log::quoted(model_.view()) << " authz=" << authz_result_
                             << " rate_limit=" << rate_limit_result_ << " rate_used=" << rate_used_
                             << " rate_max=" << rate_max_ << " rate_recover_at_ms=" << rate_recover_at_
                             << " request_body_bytes=" << body_size_ << " request_body_hash=" << body_hash_.view()
                             << " attempts=" << attempts_ << " status=" << response.status_code
                             << " response_body_bytes=" << response.body_bytes_sent
                             << " completed=" << response.completed
                             << " terminal_error=" << common::io_err_name(response.terminal_error)
                             << " duration_us=" << std::max<std::int64_t>(duration.count(), 0)
                             << " usage_input_cached=" << usage_.input_cached.value_or(-1)
                             << " usage_input_uncached=" << usage_.input_uncached.value_or(-1)
                             << " usage_output=" << usage_.output.value_or(-1)
                             << " usage_total=" << usage_.total.value_or(-1);
        if (cat_transaction_ && cat_transaction_->valid()) {
            const bool success = response.completed && response.terminal_error == common::IoErr::None &&
                                 response.status_code >= 200 && response.status_code < 400;
            (void) cat_transaction_->complete(success ? cat::status::Success : cat::status::Fail);
        }
    }

    void auth_allowed(const Bt1Principal &principal) noexcept {
        auth_result_ = "allow";
        auth_reason_ = 0;
        user_.assign(principal.username());
        kid_.assign(principal.kid());
        if (cat_transaction_ && cat_transaction_->valid()) {
            (void) cat_transaction_->add_data("user", principal.username());
            (void) cat_transaction_->add_data("kid", principal.kid());
        }
    }

    void auth_denied(const Bt1AuthError &error) noexcept {
        auth_result_ = "deny";
        auth_reason_ = static_cast<int>(error.reason);
    }

    void request_body(const mem::IoBuf &body) noexcept {
        body_size_ = body ? body.readable() : 0;
        std::array<std::uint8_t, SHA256_DIGEST_LENGTH> digest{};
        SHA256(body ? body.readable_data() : nullptr, body_size_, digest.data());
        constexpr std::string_view prefix = "sha256:";
        constexpr std::string_view digits = "0123456789abcdef";
        std::array<char, 7 + SHA256_DIGEST_LENGTH * 2> encoded{};
        std::memcpy(encoded.data(), prefix.data(), prefix.size());
        for (std::size_t i = 0; i < digest.size(); ++i) {
            encoded[prefix.size() + i * 2] = digits[digest[i] >> 4];
            encoded[prefix.size() + i * 2 + 1] = digits[digest[i] & 0x0f];
        }
        body_hash_.assign(std::string_view(encoded.data(), encoded.size()));
    }

    void model(std::string_view requested, std::string_view resolved) noexcept {
        requested_model_.assign(requested);
        model_.assign(resolved);
        authz_result_ = "allow";
        if (cat_transaction_ && cat_transaction_->valid()) {
            (void) cat_transaction_->add_data("model", resolved);
        }
    }

    void authz_denied(std::string_view requested) noexcept {
        requested_model_.assign(requested);
        authz_result_ = "deny";
    }

    void rate_limit(std::string_view result, const TokenRateLimitCheckResult &limit) noexcept {
        rate_limit_result_ = result;
        rate_used_ = limit.used_tokens;
        rate_max_ = limit.max_tokens;
        rate_recover_at_ = limit.recover_at_millis;
    }

    void rate_limit_error() noexcept { rate_limit_result_ = "error"; }

    void usage(const std::optional<LlmTokenUsage> &usage) noexcept {
        if (usage) {
            usage_.merge(*usage);
        }
    }

    void provider_attempt(const ResolvedProviderAttempt &attempt, std::size_t index, std::size_t total, int status,
                          std::chrono::microseconds duration, bool retryable, bool response_started,
                          std::string_view outcome) noexcept {
        ++attempts_;
        LOG(LOG_AUDIT, INFO) << "event=provider_attempt"
                             << " request_id=" << log::quoted(request_id_) << " attempt=" << index + 1
                             << " total_attempts=" << total << " provider=" << log::quoted(attempt.provider->name)
                             << " token_name="
                             << log::quoted(attempt.api_token ? std::string_view(attempt.api_token->name)
                                                              : std::string_view{})
                             << " protocol=" << protocol_name(protocol_)
                             << " upstream_model=" << log::quoted(attempt.protocol->model)
                             << " path=" << log::quoted(attempt.protocol->path)
                             << " provider_config_version=" << attempt.provider->config->metadata.version
                             << " fallback=" << attempt.fallback << " status=" << status
                             << " latency_us=" << std::max<std::int64_t>(duration.count(), 0)
                             << " retryable=" << retryable << " response_started=" << response_started
                             << " outcome=" << outcome;
        if (cat_transaction_ && cat_transaction_->valid()) {
            std::string data;
            data.reserve(attempt.protocol->model.size() + attempt.protocol->path.size() +
                         (attempt.api_token ? attempt.api_token->name.size() : 0) + 128);
            data.append("token_name=");
            if (attempt.api_token) {
                data.append(attempt.api_token->name);
            }
            data.append(" upstream_model=");
            data.append(attempt.protocol->model);
            data.append(" path=");
            data.append(attempt.protocol->path);
            data.append(" status=");
            std::array<char, 16> status_text{};
            auto converted = std::to_chars(status_text.data(), status_text.data() + status_text.size(), status);
            if (converted.ec == std::errc{}) {
                data.append(status_text.data(), converted.ptr);
            }
            data.append(" fallback=");
            data.append(attempt.fallback ? "true" : "false");
            data.append(" retryable=");
            data.append(retryable ? "true" : "false");
            data.append(" response_started=");
            data.append(response_started ? "true" : "false");
            data.append(" outcome=");
            data.append(outcome);
            (void) cat_transaction_->log_completed_transaction(
                    "LLM.Provider", attempt.provider->name, duration,
                    outcome == "success" ? cat::status::Success : cat::status::Fail, data);
        }
    }

private:
    http::HttpExchange *exchange_ = nullptr;
    LlmWireProtocol protocol_ = LlmWireProtocol::OpenAiChatCompletions;
    std::chrono::steady_clock::time_point started_;
    std::array<char, 1024> request_id_storage_{};
    std::string_view request_id_;
    FixedAuditText<kBt1MaxUsernameBytes> user_;
    FixedAuditText<kBt1MaxKidLength> kid_;
    FixedAuditText<128> requested_model_;
    FixedAuditText<128> model_;
    FixedAuditText<7 + SHA256_DIGEST_LENGTH * 2> body_hash_;
    std::string_view auth_result_ = "unknown";
    std::string_view authz_result_ = "unknown";
    std::string_view rate_limit_result_ = "unknown";
    int auth_reason_ = -1;
    std::size_t body_size_ = 0;
    std::size_t attempts_ = 0;
    std::int64_t rate_used_ = 0;
    std::int64_t rate_max_ = 0;
    std::int64_t rate_recover_at_ = 0;
    LlmTokenUsage usage_;
    std::optional<cat::MessageTrace> cat_trace_;
    std::optional<cat::Transaction> cat_transaction_;
};

enum class ReadRequestBodyError : std::uint8_t {
    Read,
    TooLarge,
    OutOfMemory,
};

struct ReadRequestBodyFailure {
    ReadRequestBodyError code = ReadRequestBodyError::Read;
    common::IoErr io_error = common::IoErr::None;
};

std::int64_t wall_now_millis() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
}

std::int64_t wall_now_seconds() noexcept { return wall_now_millis() / 1000; }

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

bool is_event_stream_content_type(std::string_view value) noexcept {
    const std::size_t semicolon = value.find(';');
    if (semicolon != std::string_view::npos) {
        value = value.substr(0, semicolon);
    }
    return ascii_equal_ci(trim_ascii(value), "text/event-stream");
}

bool ensure_capacity(mem::IoBuf &buffer, std::size_t required, std::size_t maximum) noexcept {
    if (buffer && buffer.capacity() >= required) {
        return true;
    }
    std::size_t capacity = std::max<std::size_t>(buffer.capacity(), 4096);
    while (capacity < required) {
        const std::size_t next = capacity > maximum / 2 ? maximum : capacity * 2;
        if (next <= capacity) {
            return false;
        }
        capacity = next;
    }
    mem::IoBuf replacement = mem::IoBuf::allocate(capacity);
    if (!replacement) {
        return false;
    }
    if (buffer && buffer.readable() > 0) {
        std::memcpy(replacement.writable_data(), buffer.readable_data(), buffer.readable());
        replacement.commit(buffer.readable());
    }
    buffer = std::move(replacement);
    return true;
}

bool append_chain(mem::IoBuf &buffer, mem::IoBufChain &chain, std::size_t maximum) noexcept {
    const std::size_t bytes = chain.readable_bytes();
    const std::size_t current = buffer ? buffer.readable() : 0;
    if (bytes > maximum || current > maximum - bytes) {
        return false;
    }
    const std::size_t required = current + bytes;
    if (!ensure_capacity(buffer, std::max<std::size_t>(required, 1), maximum)) {
        return false;
    }
    while (const mem::IoBuf *part = chain.first_readable()) {
        const std::size_t size = part->readable();
        std::memcpy(buffer.writable_data(), part->readable_data(), size);
        buffer.commit(size);
        chain.consume_and_compact(size);
    }
    return true;
}

async::Task<std::expected<mem::IoBuf, ReadRequestBodyFailure>>
read_request_body(http::HttpExchange &exchange) noexcept {
    const http::HttpBodySpec body_spec = exchange.request_body_spec();
    if (body_spec.is_content_length() && body_spec.content_length() > kMaxRequestBodyBytes) {
        co_return std::unexpected(ReadRequestBodyFailure{
                .code = ReadRequestBodyError::TooLarge,
                .io_error = common::IoErr::MessageTooLarge,
        });
    }

    mem::IoBuf body;
    if (body_spec.is_content_length()) {
        body = mem::IoBuf::allocate(std::max<std::size_t>(body_spec.content_length(), 1));
        if (!body) {
            co_return std::unexpected(ReadRequestBodyFailure{
                    .code = ReadRequestBodyError::OutOfMemory,
                    .io_error = common::IoErr::NoMem,
            });
        }
    }

    for (;;) {
        const std::size_t current = body ? body.readable() : 0;
        const std::size_t remaining = current <= kMaxRequestBodyBytes ? kMaxRequestBodyBytes - current : 0;
        auto chunk = co_await exchange.read_body(std::min<std::size_t>(kBodyChunkBytes, remaining + 1));
        if (!chunk) {
            co_return std::unexpected(ReadRequestBodyFailure{
                    .code = chunk.error() == common::IoErr::MessageTooLarge ? ReadRequestBodyError::TooLarge
                                                                            : ReadRequestBodyError::Read,
                    .io_error = chunk.error(),
            });
        }
        const bool complete = chunk->complete();
        if (chunk->readable_bytes() > remaining) {
            co_return std::unexpected(ReadRequestBodyFailure{
                    .code = ReadRequestBodyError::TooLarge,
                    .io_error = common::IoErr::MessageTooLarge,
            });
        }
        if (!append_chain(body, *chunk, kMaxRequestBodyBytes)) {
            co_return std::unexpected(ReadRequestBodyFailure{
                    .code = ReadRequestBodyError::OutOfMemory,
                    .io_error = common::IoErr::NoMem,
            });
        }
        if (complete) {
            break;
        }
    }
    if (!body) {
        body = mem::IoBuf::allocate(1);
        if (!body) {
            co_return std::unexpected(ReadRequestBodyFailure{
                    .code = ReadRequestBodyError::OutOfMemory,
                    .io_error = common::IoErr::NoMem,
            });
        }
    }
    co_return std::move(body);
}

async::Task<void> send_body(http::HttpExchange &exchange, int status_code, std::string_view content_type,
                            const mem::IoBuf &body, std::string_view request_id = {}) noexcept {
    http::HttpHeaders headers(exchange.pool());
    headers.set("Content-Type", content_type.empty() ? std::string_view("application/json") : content_type);
    if (!request_id.empty()) {
        headers.set("X-Request-Id", request_id);
    }
    const std::size_t size = body ? body.readable() : 0;
    auto sent_header = co_await exchange.send_header({
            .kind = http::OutgoingHeaderKind::Final,
            .status_code = status_code,
            .headers = &headers,
            .body = http::HttpBodySpec::ContentLength(size),
            .connection_mode = http::ResponseConnectionMode::Auto,
            .end_stream = size == 0,
    });
    if (!sent_header || size == 0) {
        co_return;
    }
    (void) co_await exchange.write_body(body.readable_data(), size, true);
}

async::Task<void> send_error(http::HttpExchange &exchange, LlmWireProtocol protocol, const LlmError &error,
                             bool allow_post = false) noexcept {
    auto encoded = encode_llm_error(protocol, error);
    if (!encoded) {
        constexpr std::string_view kOpenAiFallback =
                R"({"error":{"message":"internal server error","type":"server_error","param":null,"code":"internal_error"}})";
        constexpr std::string_view kAnthropicFallback =
                R"({"type":"error","error":{"type":"api_error","message":"internal server error"},"request_id":null})";
        const std::string_view fallback =
                protocol == LlmWireProtocol::OpenAiChatCompletions ? kOpenAiFallback : kAnthropicFallback;
        mem::IoBuf body = mem::IoBuf::allocate(fallback.size());
        if (body) {
            std::memcpy(body.writable_data(), fallback.data(), fallback.size());
            body.commit(fallback.size());
        }
        co_await send_body(exchange, 500, "application/json", body);
        co_return;
    }

    http::HttpHeaders headers(exchange.pool());
    headers.set_view("Content-Type", "application/json");
    if (allow_post) {
        headers.set_view("Allow", "POST");
    }
    auto sent_header = co_await exchange.send_header({
            .kind = http::OutgoingHeaderKind::Final,
            .status_code = error.status_code,
            .headers = &headers,
            .body = http::HttpBodySpec::ContentLength(encoded->readable()),
            .connection_mode = http::ResponseConnectionMode::Auto,
            .end_stream = encoded->readable() == 0,
    });
    if (sent_header && encoded->readable() > 0) {
        (void) co_await exchange.write_body(encoded->readable_data(), encoded->readable(), true);
    }
}

LlmError input_error(int status, std::string_view code, std::string_view message) noexcept {
    return LlmError{
            .status_code = status,
            .code = code,
            .type = "invalid_request_error",
            .message = message,
    };
}

LlmError auth_error(const Bt1AuthError &error) noexcept {
    return LlmError{
            .status_code = 401,
            .code = bt1_auth_error_name(error.kind),
            .type = "authentication_error",
            .message = bt1_auth_error_message(error.kind),
    };
}

LlmError model_error(LlmWireProtocol protocol, const ModelAuthorizationError &error) noexcept {
    switch (error.code) {
        case ModelAuthorizationErrorCode::ModelRequired:
        case ModelAuthorizationErrorCode::InvalidModelName:
            return LlmError{
                    .status_code = 400,
                    .code = "invalid_model",
                    .type = "invalid_request_error",
                    .message = error.message,
                    .field = "model",
            };
        case ModelAuthorizationErrorCode::ModelConfigUnavailable:
            return LlmError{
                    .status_code = 503,
                    .code = "model_config_unavailable",
                    .type = protocol == LlmWireProtocol::OpenAiChatCompletions ? std::string_view("server_error")
                                                                               : std::string_view("api_error"),
                    .message = error.message,
                    .field = "model",
            };
        case ModelAuthorizationErrorCode::ModelNotAvailable:
            return LlmError{
                    .status_code = 403,
                    .code = "model_not_available",
                    .type = protocol == LlmWireProtocol::OpenAiChatCompletions
                                    ? std::string_view("invalid_request_error")
                                    : std::string_view("permission_error"),
                    .message = error.message,
                    .field = "model",
            };
    }
    return {};
}

LlmError plan_error(LlmWireProtocol protocol, const ExecutionPlanError &error) noexcept {
    std::string_view code;
    switch (error.code) {
        case ExecutionPlanErrorCode::ProviderConfigUnavailable:
            code = "provider_config_unavailable";
            break;
        case ExecutionPlanErrorCode::ProviderTokenUnavailable:
            code = "provider_token_unavailable";
            break;
        case ExecutionPlanErrorCode::ProviderProtocolUnsupported:
            code = "provider_protocol_unsupported";
            break;
        case ExecutionPlanErrorCode::OutOfMemory:
            return LlmError{
                    .status_code = 500,
                    .code = "internal_error",
                    .type = protocol == LlmWireProtocol::OpenAiChatCompletions ? std::string_view("server_error")
                                                                               : std::string_view("api_error"),
                    .message = "internal server error",
            };
    }
    return LlmError{
            .status_code = 503,
            .code = code,
            .type = protocol == LlmWireProtocol::OpenAiChatCompletions ? std::string_view("server_error")
                                                                       : std::string_view("api_error"),
            .message = error.message,
            .field = "model",
    };
}

void apply_observed_provider_error(const ResolvedProviderAttempt &attempt, const ProviderErrorDecision &decision,
                                   ProviderRuntimeState::TimePoint now, AiServerMetrics::Worker &metrics,
                                   LlmWireProtocol protocol) noexcept {
    const bool was_open = attempt.runtime && attempt.runtime->provider_unavailable_until() > now;
    apply_provider_error(attempt, decision, now);
    if (!was_open && attempt.runtime && attempt.runtime->provider_unavailable_until() > now) {
        metrics.provider_circuit_open(protocol);
    }
}

class RateLimitSession {
public:
    RateLimitSession() noexcept = default;
    RateLimitSession(TokenRateLimitCoordinator &manager, RateLimitNode owner, std::string_view user,
                     std::string_view model, TokenRateLimitTicket ticket, AiServerMetrics::Worker &metrics) noexcept :
        manager_(&manager), owner_(std::move(owner)), user_(user), model_(model), ticket_(ticket), metrics_(&metrics) {}
    ~RateLimitSession() { settle_no_usage(); }

    RateLimitSession(const RateLimitSession &) = delete;
    RateLimitSession &operator=(const RateLimitSession &) = delete;
    RateLimitSession(RateLimitSession &&other) noexcept :
        manager_(other.manager_), owner_(std::move(other.owner_)), user_(other.user_), model_(other.model_),
        ticket_(other.ticket_), metrics_(other.metrics_) {
        other.manager_ = nullptr;
        other.metrics_ = nullptr;
    }
    RateLimitSession &operator=(RateLimitSession &&other) noexcept {
        if (this == &other) {
            return *this;
        }
        settle_no_usage();
        manager_ = other.manager_;
        owner_ = std::move(other.owner_);
        user_ = other.user_;
        model_ = other.model_;
        ticket_ = other.ticket_;
        metrics_ = other.metrics_;
        other.manager_ = nullptr;
        other.metrics_ = nullptr;
        return *this;
    }

    async::Task<bool> settle(std::optional<std::int64_t> total_tokens) noexcept {
        if (!manager_) {
            co_return true;
        }
        TokenRateLimitCoordinator *manager = manager_;
        AiServerMetrics::Worker *metrics = metrics_;
        manager_ = nullptr;
        metrics_ = nullptr;
        auto result =
                co_await manager->settle_and_wait(std::move(owner_), user_, model_, ticket_, total_tokens.value_or(0),
                                                  total_tokens.has_value(), wall_now_millis());
        metrics->rate_limit_settle(
                result ? (total_tokens.has_value() ? RateLimitSettleMetric::Usage : RateLimitSettleMetric::NoUsage)
                       : RateLimitSettleMetric::Error);
        co_return result.has_value();
    }

    void settle_no_usage() noexcept {
        if (!manager_) {
            return;
        }
        manager_->settle(std::move(owner_), user_, model_, ticket_, 0, false, wall_now_millis());
        metrics_->rate_limit_settle(RateLimitSettleMetric::NoUsage);
        manager_ = nullptr;
        metrics_ = nullptr;
    }

private:
    TokenRateLimitCoordinator *manager_ = nullptr;
    RateLimitNode owner_;
    std::string_view user_;
    std::string_view model_;
    TokenRateLimitTicket ticket_;
    AiServerMetrics::Worker *metrics_ = nullptr;
};

std::string_view io_buf_view(const mem::IoBuf &body) noexcept {
    return body ? std::string_view(reinterpret_cast<const char *>(body.readable_data()), body.readable())
                : std::string_view{};
}

async::Task<std::expected<BufferedProviderResponse, ProviderHttpError>>
buffer_started_response(ProviderHttpResponseStream upstream, std::size_t maximum) noexcept {
    BufferedProviderResponse response{
            .status_code = upstream.status_code(),
            .content_type = std::string(upstream.content_type()),
            .retry_after = std::string(upstream.retry_after()),
            .request_id = std::string(upstream.request_id()),
    };
    for (;;) {
        auto chunk = co_await upstream.read_body(kBodyChunkBytes, kProviderTimeout);
        if (!chunk) {
            co_return std::unexpected(ProviderHttpError{
                    .code = ProviderHttpErrorCode::ReadBody,
                    .io_error = chunk.error(),
                    .message = "failed to read provider response body",
            });
        }
        const bool complete = chunk->complete();
        const std::size_t current = response.body ? response.body.readable() : 0;
        if (chunk->readable_bytes() > maximum || current > maximum - chunk->readable_bytes()) {
            (void) upstream.abort(common::IoErr::MessageTooLarge);
            co_return std::unexpected(ProviderHttpError{
                    .code = ProviderHttpErrorCode::ResponseTooLarge,
                    .io_error = common::IoErr::MessageTooLarge,
                    .message = "provider response body is too large",
            });
        }
        if (!append_chain(response.body, *chunk, maximum)) {
            (void) upstream.abort(common::IoErr::NoMem);
            co_return std::unexpected(ProviderHttpError{
                    .code = ProviderHttpErrorCode::ReadBody,
                    .io_error = common::IoErr::NoMem,
                    .message = "failed to buffer provider response body",
            });
        }
        if (complete) {
            break;
        }
    }
    co_return std::move(response);
}

async::Task<bool> send_sse_header(http::HttpExchange &exchange, int status_code) noexcept {
    http::HttpHeaders headers(exchange.pool());
    headers.set_view("Content-Type", "text/event-stream; charset=utf-8");
    headers.set_view("Cache-Control", "no-cache");
    headers.set_view("X-Accel-Buffering", "no");
    auto sent = co_await exchange.send_header({
            .kind = http::OutgoingHeaderKind::Final,
            .status_code = status_code,
            .headers = &headers,
            .body = http::HttpBodySpec::Stream(),
            .connection_mode = http::ResponseConnectionMode::Auto,
            .end_stream = false,
    });
    co_return sent.has_value();
}

enum class SseRelayResult : std::uint8_t {
    Success,
    ProviderError,
    ClientError,
    RateLimitError,
};

async::Task<SseRelayResult> relay_sse(http::HttpExchange &exchange, ProviderHttpResponseStream &upstream,
                                      LlmWireProtocol protocol, RateLimitSession &rate_limit,
                                      std::optional<LlmTokenUsage> &usage) noexcept {
    SseParser parser(protocol);
    mem::BufPool usage_pool;
    for (;;) {
        auto chunk = co_await upstream.read_body(kBodyChunkBytes, kProviderTimeout);
        if (!chunk) {
            (void) exchange.abort(chunk.error());
            co_return SseRelayResult::ProviderError;
        }
        const bool complete = chunk->complete();
        while (const mem::IoBuf *part = chunk->first_readable()) {
            const std::string_view bytes(reinterpret_cast<const char *>(part->readable_data()), part->readable());
            if (!parser.feed(bytes, false)) {
                (void) upstream.abort(common::IoErr::Invalid);
                (void) exchange.abort(common::IoErr::Invalid);
                co_return SseRelayResult::ProviderError;
            }
            chunk->consume_and_compact(part->readable());
            for (;;) {
                const SseParseStatus status = parser.next();
                if (status == SseParseStatus::NeedMore) {
                    break;
                }
                if (status == SseParseStatus::Error) {
                    (void) upstream.abort(common::IoErr::Invalid);
                    (void) exchange.abort(common::IoErr::Invalid);
                    co_return SseRelayResult::ProviderError;
                }
                if (status == SseParseStatus::Complete) {
                    break;
                }
                const SseEventView &event = parser.event();
                if (!event.data.empty() && !event.terminal) {
                    usage_pool.reset();
                    auto extracted = extract_token_usage(protocol, event.data, true, usage_pool);
                    if (extracted) {
                        if (usage) {
                            usage->merge(*extracted);
                        } else {
                            usage = *extracted;
                        }
                    }
                }
                auto written = co_await exchange.write_body(
                        reinterpret_cast<const std::uint8_t *>(event.encoded.data()), event.encoded.size(), false);
                if (!written || *written != event.encoded.size()) {
                    (void) upstream.abort(written ? common::IoErr::Invalid : written.error());
                    co_return SseRelayResult::ClientError;
                }
            }
        }
        if (!complete) {
            continue;
        }
        if (!parser.feed({}, true)) {
            (void) upstream.abort(common::IoErr::Invalid);
            (void) exchange.abort(common::IoErr::Invalid);
            co_return SseRelayResult::ProviderError;
        }
        for (;;) {
            const SseParseStatus status = parser.next();
            if (status == SseParseStatus::Complete) {
                break;
            }
            if (status == SseParseStatus::Error || status == SseParseStatus::NeedMore) {
                (void) exchange.abort(common::IoErr::Invalid);
                co_return SseRelayResult::ProviderError;
            }
            const SseEventView &event = parser.event();
            if (!event.data.empty() && !event.terminal) {
                usage_pool.reset();
                auto extracted = extract_token_usage(protocol, event.data, true, usage_pool);
                if (extracted) {
                    if (usage) {
                        usage->merge(*extracted);
                    } else {
                        usage = *extracted;
                    }
                }
            }
            auto written = co_await exchange.write_body(reinterpret_cast<const std::uint8_t *>(event.encoded.data()),
                                                        event.encoded.size(), false);
            if (!written || *written != event.encoded.size()) {
                (void) upstream.abort(written ? common::IoErr::Invalid : written.error());
                co_return SseRelayResult::ClientError;
            }
        }

        if (!co_await rate_limit.settle(usage ? usage->total : std::optional<std::int64_t>{})) {
            (void) exchange.abort(common::IoErr::Canceled);
            co_return SseRelayResult::RateLimitError;
        }
        if (protocol == LlmWireProtocol::OpenAiChatCompletions && !parser.done_seen()) {
            constexpr std::string_view done = "data: [DONE]\n\n";
            auto written = co_await exchange.write_body(reinterpret_cast<const std::uint8_t *>(done.data()),
                                                        done.size(), true);
            co_return written && *written == done.size() ? SseRelayResult::Success : SseRelayResult::ClientError;
        }
        auto completed = co_await exchange.write_body(nullptr, 0, true);
        co_return completed ? SseRelayResult::Success : SseRelayResult::ClientError;
    }
}

} // namespace

async::Task<void> LlmRequestHandler::handle(http::HttpExchange &exchange, LlmWireProtocol protocol,
                                            std::shared_ptr<const LlmConfigSnapshot> config) noexcept {
    LlmRequestAudit audit(exchange, protocol, cat_client_);
    auto authenticated = authenticate_llm_request(exchange.request_headers(), std::move(config), wall_now_seconds());
    if (!authenticated) {
        audit.auth_denied(authenticated.error());
        co_await send_error(exchange, protocol, auth_error(authenticated.error()));
        co_return;
    }
    audit.auth_allowed(authenticated->principal());
    if (exchange.method() != http::HttpMethod::Post) {
        co_await send_error(exchange, protocol, input_error(405, "method_not_allowed", "method not allowed"), true);
        co_return;
    }
    const auto *content_type = exchange.content_type_header();
    if (!content_type || !is_json_content_type(content_type->value_view())) {
        co_await send_error(exchange, protocol,
                            input_error(415, "unsupported_media_type", "content-type must be application/json"));
        co_return;
    }

    auto raw_body = co_await read_request_body(exchange);
    if (!raw_body) {
        if (raw_body.error().code == ReadRequestBodyError::TooLarge) {
            co_await send_error(exchange, protocol, input_error(413, "request_too_large", "request body is too large"));
        } else if (raw_body.error().code == ReadRequestBodyError::OutOfMemory) {
            co_await send_error(exchange, protocol,
                                LlmError{
                                        .status_code = 500,
                                        .code = "internal_error",
                                        .type = protocol == LlmWireProtocol::OpenAiChatCompletions
                                                        ? std::string_view("server_error")
                                                        : std::string_view("api_error"),
                                        .message = "internal server error",
                                });
        } else {
            co_await send_error(exchange, protocol, input_error(400, "request_body_error", "read request body failed"));
        }
        co_return;
    }
    audit.request_body(*raw_body);

    auto parsed = ParsedLlmBody::parse(protocol, std::move(*raw_body), exchange.pool());
    if (!parsed) {
        co_await send_error(exchange, protocol, input_error(400, "invalid_json", "invalid json request body"));
        co_return;
    }
    const LlmRoutingData &routing = parsed->routing();
    const std::string_view requested_model = routing.model.is_present() ? *routing.model : std::string_view{};
    auto authorized = authorize_model(authenticated->config(), authenticated->principal().username(), requested_model);
    if (!authorized) {
        audit.authz_denied(requested_model);
        co_await send_error(exchange, protocol, model_error(protocol, authorized.error()));
        co_return;
    }
    audit.model(requested_model, authorized->model_name);

    auto route_key = build_provider_route_key(protocol, routing, authorized->route->load_balance, exchange.pool());
    if (!route_key) {
        co_await send_error(exchange, protocol,
                            LlmError{
                                    .status_code = 500,
                                    .code = "internal_error",
                                    .type = protocol == LlmWireProtocol::OpenAiChatCompletions
                                                    ? std::string_view("server_error")
                                                    : std::string_view("api_error"),
                                    .message = "internal server error",
                            });
        co_return;
    }

    auto coordinated_limit = co_await rate_limiters_->check(authenticated->principal().username(), *authorized->route,
                                                            wall_now_millis());
    if (!coordinated_limit) {
        audit.rate_limit_error();
        metrics_->rate_limit_check(RateLimitCheckMetric::Error);
        co_await send_error(exchange, protocol,
                            LlmError{
                                    .status_code = 503,
                                    .code = "rate_limit_unavailable",
                                    .type = "server_error",
                                    .message = "token rate limit service is unavailable",
                            });
        co_return;
    }
    const TokenRateLimitCheckResult &limit = coordinated_limit->result;
    metrics_->rate_limit_check(
            !limit.rule_matched ? RateLimitCheckMetric::Bypass
                                : (limit.allowed ? RateLimitCheckMetric::Allowed : RateLimitCheckMetric::Denied));
    audit.rate_limit(!limit.rule_matched ? std::string_view("bypass")
                                         : (limit.allowed ? std::string_view("allow") : std::string_view("deny")),
                     limit);
    if (!limit.allowed) {
        std::string message = "token rate limit exceeded for model: ";
        message.append(authorized->model_name);
        co_await send_error(exchange, protocol,
                            LlmError{
                                    .status_code = 429,
                                    .code = "token_rate_limit_exceeded",
                                    .type = "rate_limit_error",
                                    .message = message,
                                    .field = "model",
                            });
        co_return;
    }
    RateLimitSession rate_limit;
    if (limit.has_ticket) {
        FIBER_ASSERT(coordinated_limit->owner.has_value());
        rate_limit = RateLimitSession(*rate_limiters_, std::move(*coordinated_limit->owner),
                                      authenticated->principal().username(), authorized->model_name, limit.ticket,
                                      *metrics_);
    }

    auto plan = resolve_execution_plan(*authorized, protocol, *route_key, *provider_runtime_,
                                       event::EventLoop::current().now(), exchange.pool());
    if (!plan) {
        co_await send_error(exchange, protocol, plan_error(protocol, plan.error()));
        co_return;
    }

    const bool stream = routing.stream.is_present() && *routing.stream;
    for (std::size_t index = 0; index < plan->attempts.size(); ++index) {
        const ResolvedProviderAttempt &attempt = plan->attempts[index];
        auto rewritten =
                parsed->rewrite(attempt.protocol->model,
                                routing.stream.is_present() ? std::optional<bool>(*routing.stream) : std::nullopt,
                                event::EventLoop::current().io_buf_node_pool());
        if (!rewritten) {
            co_await send_error(exchange, protocol, input_error(400, "invalid_json", "failed to modify request body"));
            co_return;
        }

        if (stream) {
            metrics_->provider_attempt(protocol);
            const auto attempt_started = event::EventLoop::current().now();
            auto started = co_await provider_client_->start(attempt, plan->route_key, true, std::move(*rewritten),
                                                            exchange.pool());
            if (!started) {
                metrics_->provider_failure(protocol);
                const ProviderErrorDecision decision = classify_provider_transport_error(false);
                apply_observed_provider_error(attempt, decision, event::EventLoop::current().now(), *metrics_,
                                              protocol);
                audit.provider_attempt(attempt, index, plan->attempts.size(), 0,
                                       std::chrono::duration_cast<std::chrono::microseconds>(
                                               event::EventLoop::current().now() - attempt_started),
                                       decision.retryable, false, "transport_error");
                if (decision.retryable && index + 1 < plan->attempts.size()) {
                    metrics_->provider_retry(protocol);
                    continue;
                }
                co_await send_error(exchange, protocol,
                                    LlmError{
                                            .status_code = 502,
                                            .code = "provider_transport_error",
                                            .type = protocol == LlmWireProtocol::OpenAiChatCompletions
                                                            ? std::string_view("server_error")
                                                            : std::string_view("api_error"),
                                            .message = "provider request failed",
                                    });
                co_return;
            }

            if (started->status_code() < 200 || started->status_code() >= 300) {
                const int upstream_status = started->status_code();
                auto buffered = co_await buffer_started_response(std::move(*started), kMaxProviderErrorBytes);
                if (!buffered) {
                    metrics_->provider_failure(protocol);
                    const ProviderErrorDecision decision = classify_provider_transport_error(false);
                    apply_observed_provider_error(attempt, decision, event::EventLoop::current().now(), *metrics_,
                                                  protocol);
                    audit.provider_attempt(attempt, index, plan->attempts.size(), upstream_status,
                                           std::chrono::duration_cast<std::chrono::microseconds>(
                                                   event::EventLoop::current().now() - attempt_started),
                                           decision.retryable, false, "response_read_error");
                    if (decision.retryable && index + 1 < plan->attempts.size()) {
                        metrics_->provider_retry(protocol);
                        continue;
                    }
                    co_await send_error(exchange, protocol,
                                        LlmError{
                                                .status_code = 502,
                                                .code = "provider_transport_error",
                                                .type = protocol == LlmWireProtocol::OpenAiChatCompletions
                                                                ? std::string_view("server_error")
                                                                : std::string_view("api_error"),
                                                .message = "provider response failed",
                                        });
                    co_return;
                }
                const ProviderErrorDecision decision = classify_provider_response(
                        protocol, buffered->status_code, buffered->retry_after, io_buf_view(buffered->body),
                        plan->load_balance, false, exchange.pool());
                apply_observed_provider_error(attempt, decision, event::EventLoop::current().now(), *metrics_,
                                              protocol);
                metrics_->provider_failure(protocol);
                audit.provider_attempt(attempt, index, plan->attempts.size(), buffered->status_code,
                                       std::chrono::duration_cast<std::chrono::microseconds>(
                                               event::EventLoop::current().now() - attempt_started),
                                       decision.retryable, false, "upstream_error");
                if (decision.retryable && index + 1 < plan->attempts.size()) {
                    metrics_->provider_retry(protocol);
                    continue;
                }
                if (!buffered->body || buffered->body.readable() == 0) {
                    co_await send_error(exchange, protocol,
                                        LlmError{
                                                .status_code = 502,
                                                .code = "upstream_invalid_error_response",
                                                .type = "api_error",
                                                .message = "provider error response is empty",
                                        });
                    co_return;
                }
                co_await send_body(exchange, buffered->status_code, buffered->content_type, buffered->body,
                                   buffered->request_id);
                co_return;
            }
            if (!is_event_stream_content_type(started->content_type())) {
                metrics_->provider_failure(protocol);
                const ProviderErrorDecision decision = classify_provider_transport_error(true);
                apply_observed_provider_error(attempt, decision, event::EventLoop::current().now(), *metrics_,
                                              protocol);
                audit.provider_attempt(attempt, index, plan->attempts.size(), started->status_code(),
                                       std::chrono::duration_cast<std::chrono::microseconds>(
                                               event::EventLoop::current().now() - attempt_started),
                                       false, false, "invalid_content_type");
                (void) started->abort(common::IoErr::Invalid);
                co_await send_error(exchange, protocol,
                                    LlmError{
                                            .status_code = 502,
                                            .code = "upstream_invalid_response",
                                            .type = "api_error",
                                            .message = "provider did not return an event stream",
                                    });
                co_return;
            }
            if (!co_await send_sse_header(exchange, started->status_code())) {
                audit.provider_attempt(attempt, index, plan->attempts.size(), started->status_code(),
                                       std::chrono::duration_cast<std::chrono::microseconds>(
                                               event::EventLoop::current().now() - attempt_started),
                                       false, false, "client_header_error");
                (void) started->abort(common::IoErr::Canceled);
                co_return;
            }
            std::optional<LlmTokenUsage> usage;
            const SseRelayResult relay_result = co_await relay_sse(exchange, *started, protocol, rate_limit, usage);
            audit.usage(usage);
            if (relay_result == SseRelayResult::Success) {
                attempt.runtime->record_success(attempt.api_token ? attempt.api_token->name : std::string_view{});
            } else {
                metrics_->sse_failure(protocol);
                if (relay_result == SseRelayResult::ProviderError) {
                    metrics_->provider_failure(protocol);
                    apply_observed_provider_error(attempt, classify_provider_transport_error(true),
                                                  event::EventLoop::current().now(), *metrics_, protocol);
                }
            }
            const std::string_view outcome =
                    relay_result == SseRelayResult::Success         ? std::string_view("success")
                    : relay_result == SseRelayResult::ProviderError ? std::string_view("stream_error")
                    : relay_result == SseRelayResult::ClientError   ? std::string_view("client_stream_error")
                                                                    : std::string_view("rate_limit_settle_error");
            audit.provider_attempt(attempt, index, plan->attempts.size(), started->status_code(),
                                   std::chrono::duration_cast<std::chrono::microseconds>(
                                           event::EventLoop::current().now() - attempt_started),
                                   false, true, outcome);
            co_return;
        }

        metrics_->provider_attempt(protocol);
        const auto attempt_started = event::EventLoop::current().now();
        auto response = co_await provider_client_->execute_buffered(
                attempt, plan->route_key, false, std::move(*rewritten), exchange.pool(), kMaxProviderResponseBytes);
        if (!response) {
            metrics_->provider_failure(protocol);
            const ProviderErrorDecision decision = classify_provider_transport_error(false);
            apply_observed_provider_error(attempt, decision, event::EventLoop::current().now(), *metrics_, protocol);
            audit.provider_attempt(attempt, index, plan->attempts.size(), 0,
                                   std::chrono::duration_cast<std::chrono::microseconds>(
                                           event::EventLoop::current().now() - attempt_started),
                                   decision.retryable, false, "transport_error");
            if (decision.retryable && index + 1 < plan->attempts.size()) {
                metrics_->provider_retry(protocol);
                continue;
            }
            co_await send_error(exchange, protocol,
                                LlmError{
                                        .status_code = 502,
                                        .code = "provider_transport_error",
                                        .type = protocol == LlmWireProtocol::OpenAiChatCompletions
                                                        ? std::string_view("server_error")
                                                        : std::string_view("api_error"),
                                        .message = "provider request failed",
                                });
            co_return;
        }

        if (response->status_code >= 200 && response->status_code < 300) {
            attempt.runtime->record_success(attempt.api_token ? attempt.api_token->name : std::string_view{});
            mem::BufPool usage_pool;
            auto usage = extract_token_usage(protocol, io_buf_view(response->body), false, usage_pool);
            audit.usage(usage);
            audit.provider_attempt(attempt, index, plan->attempts.size(), response->status_code,
                                   std::chrono::duration_cast<std::chrono::microseconds>(
                                           event::EventLoop::current().now() - attempt_started),
                                   false, false, "success");
            if (!co_await rate_limit.settle(usage ? usage->total : std::optional<std::int64_t>{})) {
                co_await send_error(exchange, protocol,
                                    LlmError{
                                            .status_code = 503,
                                            .code = "rate_limit_unavailable",
                                            .type = protocol == LlmWireProtocol::OpenAiChatCompletions
                                                            ? std::string_view("server_error")
                                                            : std::string_view("api_error"),
                                            .message = "token rate limit service is unavailable",
                                    });
                co_return;
            }
            co_await send_body(exchange, response->status_code, response->content_type, response->body,
                               response->request_id);
            co_return;
        }

        const ProviderErrorDecision decision =
                classify_provider_response(protocol, response->status_code, response->retry_after,
                                           io_buf_view(response->body), plan->load_balance, false, exchange.pool());
        apply_observed_provider_error(attempt, decision, event::EventLoop::current().now(), *metrics_, protocol);
        metrics_->provider_failure(protocol);
        audit.provider_attempt(attempt, index, plan->attempts.size(), response->status_code,
                               std::chrono::duration_cast<std::chrono::microseconds>(event::EventLoop::current().now() -
                                                                                     attempt_started),
                               decision.retryable, false, "upstream_error");
        if (decision.retryable && index + 1 < plan->attempts.size()) {
            metrics_->provider_retry(protocol);
            continue;
        }
        if (!response->body || response->body.readable() == 0) {
            co_await send_error(exchange, protocol,
                                LlmError{
                                        .status_code = 502,
                                        .code = "upstream_invalid_error_response",
                                        .type = "api_error",
                                        .message = "provider error response is empty",
                                });
            co_return;
        }
        co_await send_body(exchange, response->status_code, response->content_type, response->body,
                           response->request_id);
        co_return;
    }

    co_await send_error(exchange, protocol,
                        LlmError{
                                .status_code = 503,
                                .code = "provider_config_unavailable",
                                .type = protocol == LlmWireProtocol::OpenAiChatCompletions
                                                ? std::string_view("server_error")
                                                : std::string_view("api_error"),
                                .message = "provider config is unavailable",
                                .field = "model",
                        });
}

} // namespace fiber::ai_server
