#include "RateLimitHttpCodec.h"

#include <fiber/common/json/JsonEncode.h>
#include <fiber/common/json/JsonParser.h>
#include <fiber/common/json/JsonStructDecode.h>

FIBER_JSON_STRUCT(fiber::ai_server::RateLimitCheckRequest, FIBER_JSON_NAMED_FIELD(user_id, "userId"),
                  FIBER_JSON_NAMED_FIELD(model_name, "modelName"),
                  FIBER_JSON_NAMED_FIELD(rule_revision, "ruleRevision"),
                  FIBER_JSON_NAMED_FIELD(window_duration_millis, "windowDurationMillis"),
                  FIBER_JSON_NAMED_FIELD(max_tokens_per_window, "maxTokensPerWindow"));

FIBER_JSON_STRUCT(fiber::ai_server::RateLimitTicketPayload, FIBER_JSON_NAMED_FIELD(rule_revision, "ruleRevision"),
                  FIBER_JSON_FIELD(generation), FIBER_JSON_NAMED_FIELD(window_start_millis, "windowStartMillis"));

FIBER_JSON_STRUCT(fiber::ai_server::RateLimitCheckResponse, FIBER_JSON_NAMED_FIELD(rule_matched, "ruleMatched"),
                  FIBER_JSON_FIELD(allowed), FIBER_JSON_NAMED_FIELD(used_tokens, "usedTokens"),
                  FIBER_JSON_NAMED_FIELD(max_tokens, "maxTokens"),
                  FIBER_JSON_NAMED_FIELD(recover_at_millis, "recoverAtMillis"), FIBER_JSON_FIELD(ticket));

FIBER_JSON_STRUCT(fiber::ai_server::RateLimitSettleRequest, FIBER_JSON_NAMED_FIELD(user_id, "userId"),
                  FIBER_JSON_NAMED_FIELD(model_name, "modelName"), FIBER_JSON_FIELD(ticket), FIBER_JSON_FIELD(tokens),
                  FIBER_JSON_NAMED_FIELD(count_usage, "countUsage"));

FIBER_JSON_STRUCT(fiber::ai_server::RateLimitSettleResponse, FIBER_JSON_FIELD(applied), FIBER_JSON_FIELD(stale),
                  FIBER_JSON_NAMED_FIELD(usage_counted, "usageCounted"),
                  FIBER_JSON_NAMED_FIELD(used_tokens, "usedTokens"),
                  FIBER_JSON_NAMED_FIELD(recover_at_millis, "recoverAtMillis"));

namespace fiber::ai_server {
namespace {

class BoundedStringSink final : public json::OutputSink {
public:
    explicit BoundedStringSink(std::string &output) noexcept : output_(&output) {}

    [[nodiscard]] bool write(const char *data, std::size_t len) override {
        if ((!data && len != 0) || len > kMaxRateLimitHttpBodyBytes - output_->size()) {
            too_large_ = true;
            return false;
        }
        output_->append(data ? data : "", len);
        return true;
    }

    [[nodiscard]] bool too_large() const noexcept { return too_large_; }

private:
    std::string *output_ = nullptr;
    bool too_large_ = false;
};

bool ok(json::Generator::Result result) noexcept { return result == json::Generator::Result::OK; }

RateLimitPayloadError encode_error(const BoundedStringSink &sink) noexcept {
    return RateLimitPayloadError{
            .code = sink.too_large() ? RateLimitPayloadErrorCode::TooLarge : RateLimitPayloadErrorCode::EncodeFailed,
            .message = sink.too_large() ? "rate limit payload is too large" : "failed to encode rate limit payload",
    };
}

template<typename T>
std::expected<T, RateLimitPayloadError> decode(std::string_view body, mem::BufPool &pool) noexcept {
    if (body.empty()) {
        return std::unexpected(RateLimitPayloadError{
                .code = RateLimitPayloadErrorCode::EmptyBody,
                .message = "request body is empty",
        });
    }
    if (body.size() > kMaxRateLimitHttpBodyBytes) {
        return std::unexpected(RateLimitPayloadError{
                .code = RateLimitPayloadErrorCode::TooLarge,
                .message = "request body is too large",
        });
    }
    json::JsonParser parser;
    if (!parser.feed(body.data(), body.size())) {
        return std::unexpected(RateLimitPayloadError{
                .code = RateLimitPayloadErrorCode::InvalidJson,
                .message = "invalid json request body",
        });
    }
    parser.finish();
    T output;
    auto value_parser = [](json::JsonParser &value_parser, mem::BufPool &value_pool, T &value) noexcept {
        return json::parse_value(value_parser, value_pool, value);
    };
    if (json::parse_document(parser, pool, output, value_parser) != json::ParseStatus::Done) {
        return std::unexpected(RateLimitPayloadError{
                .code = RateLimitPayloadErrorCode::InvalidJson,
                .message = "invalid json request body",
        });
    }
    return output;
}

json::Generator::Result encode_ticket(json::Generator &generator, const std::optional<RateLimitTicketPayload> &ticket) {
    if (!ticket) {
        return generator.null_value();
    }
    auto result = generator.map_open();
    if (ok(result)) {
        result = generator.string("ruleRevision", 12);
    }
    if (ok(result)) {
        result = generator.integer(ticket->rule_revision);
    }
    if (ok(result)) {
        result = generator.string("generation", 10);
    }
    if (ok(result)) {
        result = generator.integer(static_cast<std::int64_t>(ticket->generation));
    }
    if (ok(result)) {
        result = generator.string("windowStartMillis", 17);
    }
    if (ok(result)) {
        result = generator.integer(ticket->window_start_millis);
    }
    if (ok(result)) {
        result = generator.map_close();
    }
    return result;
}

json::Generator::Result key(json::Generator &generator, std::string_view name) {
    return generator.string(name.data(), name.size());
}

json::Generator::Result text(json::Generator &generator, std::string_view value) {
    return generator.string(value.data(), value.size());
}

std::expected<std::string, RateLimitPayloadError> finish_encode(BoundedStringSink &sink, std::string output,
                                                                json::Generator::Result result) {
    if (!ok(result)) {
        return std::unexpected(encode_error(sink));
    }
    return output;
}

} // namespace

std::expected<RateLimitCheckRequest, RateLimitPayloadError>
decode_rate_limit_check_request(std::string_view body, mem::BufPool &pool) noexcept {
    auto decoded = decode<RateLimitCheckRequest>(body, pool);
    if (!decoded) {
        return decoded;
    }
    if (decoded->user_id.empty() || decoded->model_name.empty() || decoded->window_duration_millis <= 0 ||
        decoded->max_tokens_per_window < 0) {
        return std::unexpected(RateLimitPayloadError{
                .code = RateLimitPayloadErrorCode::InvalidValue,
                .message = "invalid rate limit check",
        });
    }
    return decoded;
}

std::expected<RateLimitCheckResponse, RateLimitPayloadError>
decode_rate_limit_check_response(std::string_view body, mem::BufPool &pool) noexcept {
    return decode<RateLimitCheckResponse>(body, pool);
}

std::expected<RateLimitSettleRequest, RateLimitPayloadError>
decode_rate_limit_settle_request(std::string_view body, mem::BufPool &pool) noexcept {
    auto decoded = decode<RateLimitSettleRequest>(body, pool);
    if (!decoded) {
        return decoded;
    }
    if (decoded->user_id.empty() || decoded->model_name.empty() || !decoded->ticket || decoded->tokens < 0 ||
        (!decoded->count_usage && decoded->tokens != 0)) {
        return std::unexpected(RateLimitPayloadError{
                .code = RateLimitPayloadErrorCode::InvalidValue,
                .message = "invalid rate limit settlement",
        });
    }
    return decoded;
}

std::expected<RateLimitSettleResponse, RateLimitPayloadError>
decode_rate_limit_settle_response(std::string_view body, mem::BufPool &pool) noexcept {
    return decode<RateLimitSettleResponse>(body, pool);
}

std::expected<std::string, RateLimitPayloadError> encode_rate_limit_check_request(const RateLimitCheckRequest &value) {
    std::string output;
    output.reserve(128);
    BoundedStringSink sink(output);
    json::Generator generator(sink);
    generator.set_option(json::Generator::Option::ValidateUtf8);
    auto result = generator.map_open();
    if (ok(result)) {
        result = key(generator, "userId");
    }
    if (ok(result)) {
        result = text(generator, value.user_id);
    }
    if (ok(result)) {
        result = key(generator, "modelName");
    }
    if (ok(result)) {
        result = text(generator, value.model_name);
    }
    if (ok(result)) {
        result = key(generator, "ruleRevision");
    }
    if (ok(result)) {
        result = generator.integer(value.rule_revision);
    }
    if (ok(result)) {
        result = key(generator, "windowDurationMillis");
    }
    if (ok(result)) {
        result = generator.integer(value.window_duration_millis);
    }
    if (ok(result)) {
        result = key(generator, "maxTokensPerWindow");
    }
    if (ok(result)) {
        result = generator.integer(value.max_tokens_per_window);
    }
    if (ok(result)) {
        result = generator.map_close();
    }
    return finish_encode(sink, std::move(output), result);
}

std::expected<std::string, RateLimitPayloadError>
encode_rate_limit_check_response(const RateLimitCheckResponse &value) {
    std::string output;
    output.reserve(192);
    BoundedStringSink sink(output);
    json::Generator generator(sink);
    auto result = generator.map_open();
#define ENCODE_FIELD_NAME(NAME)                                                                                        \
    if (ok(result)) {                                                                                                  \
        result = key(generator, NAME);                                                                                 \
    }
    ENCODE_FIELD_NAME("ruleMatched")
    if (ok(result)) {
        result = generator.bool_value(value.rule_matched);
    }
    ENCODE_FIELD_NAME("allowed")
    if (ok(result)) {
        result = generator.bool_value(value.allowed);
    }
    ENCODE_FIELD_NAME("usedTokens")
    if (ok(result)) {
        result = generator.integer(value.used_tokens);
    }
    ENCODE_FIELD_NAME("maxTokens")
    if (ok(result)) {
        result = generator.integer(value.max_tokens);
    }
    ENCODE_FIELD_NAME("recoverAtMillis")
    if (ok(result)) {
        result = generator.integer(value.recover_at_millis);
    }
    ENCODE_FIELD_NAME("ticket")
    if (ok(result)) {
        result = encode_ticket(generator, value.ticket);
    }
    if (ok(result)) {
        result = generator.map_close();
    }
#undef ENCODE_FIELD_NAME
    return finish_encode(sink, std::move(output), result);
}

std::expected<std::string, RateLimitPayloadError>
encode_rate_limit_settle_request(const RateLimitSettleRequest &value) {
    std::string output;
    output.reserve(192);
    BoundedStringSink sink(output);
    json::Generator generator(sink);
    generator.set_option(json::Generator::Option::ValidateUtf8);
    auto result = generator.map_open();
#define ENCODE_FIELD_NAME(NAME)                                                                                        \
    if (ok(result)) {                                                                                                  \
        result = key(generator, NAME);                                                                                 \
    }
    ENCODE_FIELD_NAME("userId")
    if (ok(result)) {
        result = text(generator, value.user_id);
    }
    ENCODE_FIELD_NAME("modelName")
    if (ok(result)) {
        result = text(generator, value.model_name);
    }
    ENCODE_FIELD_NAME("ticket")
    if (ok(result)) {
        result = encode_ticket(generator, value.ticket);
    }
    ENCODE_FIELD_NAME("tokens")
    if (ok(result)) {
        result = generator.integer(value.tokens);
    }
    ENCODE_FIELD_NAME("countUsage")
    if (ok(result)) {
        result = generator.bool_value(value.count_usage);
    }
    if (ok(result)) {
        result = generator.map_close();
    }
#undef ENCODE_FIELD_NAME
    return finish_encode(sink, std::move(output), result);
}

std::expected<std::string, RateLimitPayloadError>
encode_rate_limit_settle_response(const RateLimitSettleResponse &value) {
    std::string output;
    output.reserve(160);
    BoundedStringSink sink(output);
    json::Generator generator(sink);
    auto result = generator.map_open();
#define ENCODE_FIELD_NAME(NAME)                                                                                        \
    if (ok(result)) {                                                                                                  \
        result = key(generator, NAME);                                                                                 \
    }
    ENCODE_FIELD_NAME("applied")
    if (ok(result)) {
        result = generator.bool_value(value.applied);
    }
    ENCODE_FIELD_NAME("stale")
    if (ok(result)) {
        result = generator.bool_value(value.stale);
    }
    ENCODE_FIELD_NAME("usageCounted")
    if (ok(result)) {
        result = generator.bool_value(value.usage_counted);
    }
    ENCODE_FIELD_NAME("usedTokens")
    if (ok(result)) {
        result = generator.integer(value.used_tokens);
    }
    ENCODE_FIELD_NAME("recoverAtMillis")
    if (ok(result)) {
        result = generator.integer(value.recover_at_millis);
    }
    if (ok(result)) {
        result = generator.map_close();
    }
#undef ENCODE_FIELD_NAME
    return finish_encode(sink, std::move(output), result);
}

RateLimitCheckResponse to_http_response(const TokenRateLimitCheckResult &value) noexcept {
    RateLimitCheckResponse output{
            .rule_matched = value.rule_matched,
            .allowed = value.allowed,
            .used_tokens = value.used_tokens,
            .max_tokens = value.max_tokens,
            .recover_at_millis = value.recover_at_millis,
    };
    if (value.has_ticket) {
        output.ticket = RateLimitTicketPayload{
                .rule_revision = value.ticket.rule_revision,
                .generation = value.ticket.generation,
                .window_start_millis = value.ticket.window_start_millis,
        };
    }
    return output;
}

RateLimitSettleResponse to_http_response(const TokenRateLimitSettleResult &value) noexcept {
    return RateLimitSettleResponse{
            .applied = value.applied,
            .stale = !value.applied,
            .usage_counted = value.usage_counted,
            .used_tokens = value.used_tokens,
            .recover_at_millis = value.recover_at_millis,
    };
}

} // namespace fiber::ai_server
