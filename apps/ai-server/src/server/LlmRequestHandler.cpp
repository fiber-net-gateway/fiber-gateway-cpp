#include "LlmRequestHandler.h"

#include "../audit/LlmAuditLog.h"
#include "../auth/LlmRequestAuthenticator.h"
#include "../observability/AiServerCatRequest.h"
#include "../observability/AiServerLogCategories.h"
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
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <openssl/sha.h>

#include <common/Assert.h>
#include <common/IoError.h>
#include <common/json/JsonEncode.h>
#include <common/json/JsonPath.h>
#include <event/EventLoop.h>
#include <fiber/cat/Status.h>
#include <http/HttpBodySpec.h>
#include <http/HttpExchange.h>
#include <http/HttpExchangeIo.h>
#include <http/HttpHeaders.h>
#include <log/Log.h>

namespace fiber::ai_server {
namespace {

DEFINE_LOGGER(LOG_LLM, kAiServerLlmLogger);
DEFINE_LOGGER(LOG_LLM_AUDIT, kAiServerAuditLogger);

constexpr std::size_t kMaxRequestBodyBytes = 4 * 1024 * 1024;
constexpr std::size_t kMaxProviderErrorBytes = 4 * 1024 * 1024;
constexpr std::size_t kMaxProviderResponseBytes = 32 * 1024 * 1024;
constexpr std::size_t kBodyChunkBytes = 64 * 1024;
constexpr std::size_t kAuditRecordMetadataReserveBytes = 1024 * 1024;
constexpr std::chrono::seconds kProviderTimeout{300};

std::int64_t wall_now_millis() noexcept;

std::string_view protocol_name(LlmWireProtocol protocol) noexcept {
    return protocol == LlmWireProtocol::OpenAiChatCompletions ? std::string_view("openai")
                                                              : std::string_view("anthropic");
}

void add_cat_integer(cat::Event &event, std::string_view key, const std::optional<std::int64_t> &value) noexcept {
    if (!value) {
        return;
    }
    std::array<char, std::numeric_limits<std::int64_t>::digits10 + 3> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), *value);
    if (converted.ec == std::errc{}) {
        (void) event.add_data(key,
                              std::string_view(buffer.data(), static_cast<std::size_t>(converted.ptr - buffer.data())));
    }
}

void append_cat_duration(std::string &data, std::string_view key, std::chrono::microseconds duration) noexcept {
    std::array<char, std::numeric_limits<std::int64_t>::digits10 + 3> buffer{};
    const auto value = std::max<std::int64_t>(duration.count(), 0);
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (converted.ec != std::errc{}) {
        return;
    }
    data.push_back(' ');
    data.append(key);
    data.push_back('=');
    data.append(buffer.data(), converted.ptr);
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
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

private:
    std::array<char, Capacity> value_{};
    std::size_t size_ = 0;
};

void assign_sha256(std::string_view input, FixedAuditText<7 + SHA256_DIGEST_LENGTH * 2> &output) noexcept {
    std::array<std::uint8_t, SHA256_DIGEST_LENGTH> digest{};
    (void) SHA256(reinterpret_cast<const unsigned char *>(input.data()), input.size(), digest.data());
    constexpr std::string_view prefix = "sha256:";
    constexpr std::string_view digits = "0123456789abcdef";
    std::array<char, 7 + SHA256_DIGEST_LENGTH * 2> encoded{};
    std::memcpy(encoded.data(), prefix.data(), prefix.size());
    for (std::size_t i = 0; i < digest.size(); ++i) {
        encoded[prefix.size() + i * 2] = digits[digest[i] >> 4];
        encoded[prefix.size() + i * 2 + 1] = digits[digest[i] & 0x0f];
    }
    output.assign(std::string_view(encoded.data(), encoded.size()));
}

std::string_view auth_failure_reason_name(Bt1AuthFailureReason reason) noexcept {
    switch (reason) {
        case Bt1AuthFailureReason::MissingCredential:
            return "missing_credential";
        case Bt1AuthFailureReason::InvalidAuthorizationScheme:
            return "invalid_authorization_scheme";
        case Bt1AuthFailureReason::EmptyToken:
            return "empty_token";
        case Bt1AuthFailureReason::TokenTooLong:
            return "token_too_long";
        case Bt1AuthFailureReason::InvalidSegmentCount:
            return "invalid_segment_count";
        case Bt1AuthFailureReason::InvalidVersion:
            return "invalid_version";
        case Bt1AuthFailureReason::InvalidKid:
            return "invalid_kid";
        case Bt1AuthFailureReason::UnknownKid:
            return "unknown_kid";
        case Bt1AuthFailureReason::InvalidUserEncoding:
            return "invalid_user_encoding";
        case Bt1AuthFailureReason::InvalidRandomEncoding:
            return "invalid_random_encoding";
        case Bt1AuthFailureReason::InvalidMacEncoding:
            return "invalid_mac_encoding";
        case Bt1AuthFailureReason::InvalidExpiration:
            return "invalid_expiration";
        case Bt1AuthFailureReason::Expired:
            return "expired";
        case Bt1AuthFailureReason::MacMismatch:
            return "mac_mismatch";
        case Bt1AuthFailureReason::InvalidUsername:
            return "invalid_username";
        case Bt1AuthFailureReason::AuthConfigUnavailable:
            return "auth_config_unavailable";
        case Bt1AuthFailureReason::CryptoFailure:
            return "crypto_failure";
    }
    return "unknown";
}

std::size_t saturating_add(std::size_t left, std::size_t right) noexcept {
    return left > std::numeric_limits<std::size_t>::max() - right ? std::numeric_limits<std::size_t>::max()
                                                                  : left + right;
}

std::size_t utf8_sequence_size(std::string_view input, std::size_t offset) noexcept {
    const auto first = static_cast<unsigned char>(input[offset]);
    if (first < 0x80) {
        return 1;
    }
    std::size_t size = 0;
    std::uint32_t codepoint = 0;
    if ((first & 0xe0) == 0xc0) {
        size = 2;
        codepoint = first & 0x1f;
    } else if ((first & 0xf0) == 0xe0) {
        size = 3;
        codepoint = first & 0x0f;
    } else if ((first & 0xf8) == 0xf0) {
        size = 4;
        codepoint = first & 0x07;
    } else {
        return 0;
    }
    if (size > input.size() - offset) {
        return 0;
    }
    for (std::size_t i = 1; i < size; ++i) {
        const auto next = static_cast<unsigned char>(input[offset + i]);
        if ((next & 0xc0) != 0x80) {
            return 0;
        }
        codepoint = (codepoint << 6) | (next & 0x3f);
    }
    if ((size == 2 && codepoint < 0x80) || (size == 3 && codepoint < 0x800) ||
        (size == 4 && (codepoint < 0x10000 || codepoint > 0x10ffff)) || (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
        return 0;
    }
    return size;
}

bool is_valid_utf8(std::string_view input) noexcept {
    std::size_t offset = 0;
    while (offset < input.size()) {
        const std::size_t sequence = utf8_sequence_size(input, offset);
        if (sequence == 0) {
            return false;
        }
        offset += sequence;
    }
    return true;
}

std::size_t json_string_content_size(std::string_view value) noexcept {
    std::size_t size = 0;
    std::size_t offset = 0;
    while (offset < value.size()) {
        const auto ch = static_cast<unsigned char>(value[offset]);
        const std::size_t sequence = utf8_sequence_size(value, offset);
        if (sequence == 0) {
            return std::numeric_limits<std::size_t>::max();
        }
        std::size_t encoded = sequence;
        if (ch < 0x20) {
            encoded = ch == '\b' || ch == '\f' || ch == '\n' || ch == '\r' || ch == '\t' ? 2 : 6;
        } else if (ch == '"' || ch == '\\') {
            encoded = 2;
        }
        size = saturating_add(size, encoded);
        offset += sequence;
    }
    return size;
}

class AuditLogSink final : public json::OutputSink {
public:
    AuditLogSink(log::LogLine &line, std::size_t max_bytes) noexcept : line_(&line), max_bytes_(max_bytes) {}

    [[nodiscard]] bool write(const char *data, std::size_t size) override {
        if ((data == nullptr && size != 0) || size_ > max_bytes_ || size > max_bytes_ - size_ ||
            (size != 0 && (std::memchr(data, '\n', size) != nullptr || std::memchr(data, '\r', size) != nullptr))) {
            return false;
        }
        if (size != 0 && !line_->append_raw(std::string_view(data, size))) {
            return false;
        }
        size_ += size;
        return true;
    }

private:
    log::LogLine *line_ = nullptr;
    std::size_t max_bytes_ = 0;
    std::size_t size_ = 0;
};

struct Base64AuditCursor {
    std::string_view input;
    std::size_t offset = 0;
    std::array<char, 4096> output{};
};

bool next_base64_audit_chunk(void *opaque, const char *&data, std::size_t &size, bool &done) noexcept {
    static constexpr char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    auto &cursor = *static_cast<Base64AuditCursor *>(opaque);
    if (cursor.offset == cursor.input.size()) {
        data = nullptr;
        size = 0;
        done = true;
        return true;
    }

    const std::size_t remaining = cursor.input.size() - cursor.offset;
    const std::size_t input_bytes = std::min<std::size_t>(remaining, (cursor.output.size() / 4) * 3);
    const auto *input = reinterpret_cast<const unsigned char *>(cursor.input.data() + cursor.offset);
    std::size_t input_offset = 0;
    std::size_t output_offset = 0;
    while (input_offset + 3 <= input_bytes) {
        const std::uint32_t value = (static_cast<std::uint32_t>(input[input_offset]) << 16) |
                                    (static_cast<std::uint32_t>(input[input_offset + 1]) << 8) |
                                    static_cast<std::uint32_t>(input[input_offset + 2]);
        cursor.output[output_offset++] = digits[(value >> 18) & 0x3f];
        cursor.output[output_offset++] = digits[(value >> 12) & 0x3f];
        cursor.output[output_offset++] = digits[(value >> 6) & 0x3f];
        cursor.output[output_offset++] = digits[value & 0x3f];
        input_offset += 3;
    }
    const std::size_t tail = input_bytes - input_offset;
    if (tail != 0) {
        std::uint32_t value = static_cast<std::uint32_t>(input[input_offset]) << 16;
        if (tail == 2) {
            value |= static_cast<std::uint32_t>(input[input_offset + 1]) << 8;
        }
        cursor.output[output_offset++] = digits[(value >> 18) & 0x3f];
        cursor.output[output_offset++] = digits[(value >> 12) & 0x3f];
        cursor.output[output_offset++] = tail == 2 ? digits[(value >> 6) & 0x3f] : '=';
        cursor.output[output_offset++] = '=';
    }
    cursor.offset += input_bytes;
    data = cursor.output.data();
    size = output_offset;
    done = cursor.offset == cursor.input.size();
    return true;
}

class AuditJsonWriter {
public:
    explicit AuditJsonWriter(json::OutputSink &output) noexcept : generator_(output) {
        generator_.set_option(json::Generator::Option::ValidateUtf8);
    }

    [[nodiscard]] bool good() const noexcept { return good_; }

    void object_open() noexcept { apply(generator_.map_open()); }
    void object_close() noexcept { apply(generator_.map_close()); }
    void array_open() noexcept { apply(generator_.array_open()); }
    void array_close() noexcept { apply(generator_.array_close()); }
    void key(std::string_view value) noexcept { text(value); }
    void text(std::string_view value) noexcept {
        if (good_) {
            apply(generator_.string(value.data(), value.size()));
        }
    }
    void text(const LlmAuditBuffer &value) noexcept {
        if (!good_) {
            return;
        }
        auto cursor = value.cursor();
        apply(generator_.string_from_chunks(&LlmAuditBuffer::next_chunk, &cursor));
    }
    void binary_text(std::string_view value) noexcept {
        if (!good_) {
            return;
        }
        Base64AuditCursor cursor{.input = value};
        apply(generator_.string_from_chunks(&next_base64_audit_chunk, &cursor));
    }
    void integer(std::int64_t value) noexcept { apply(generator_.integer(value)); }
    void boolean(bool value) noexcept { apply(generator_.bool_value(value)); }
    void null() noexcept { apply(generator_.null_value()); }

    void field(std::string_view name, std::string_view value) noexcept {
        key(name);
        text(value);
    }
    void field(std::string_view name, const char *value) noexcept {
        field(name, value ? std::string_view(value) : std::string_view{});
    }
    void field(std::string_view name, const LlmAuditBuffer &value) noexcept {
        key(name);
        text(value);
    }
    void field(std::string_view name, std::int64_t value) noexcept {
        key(name);
        integer(value);
    }
    template<std::integral T>
        requires(!std::same_as<std::remove_cv_t<T>, bool> && !std::same_as<std::remove_cv_t<T>, std::int64_t>)
    void field(std::string_view name, T value) noexcept {
        field(name, static_cast<std::int64_t>(value));
    }
    void field(std::string_view name, bool value) noexcept {
        key(name);
        boolean(value);
    }
    void null_field(std::string_view name) noexcept {
        key(name);
        null();
    }
    void optional_field(std::string_view name, const std::optional<std::int64_t> &value) noexcept {
        key(name);
        if (value) {
            integer(*value);
        } else {
            null();
        }
    }

private:
    void apply(json::Generator::Result result) noexcept {
        if (result != json::Generator::Result::OK) {
            good_ = false;
        }
    }

    json::Generator generator_;
    bool good_ = true;
};

enum class OutputAuditAction : std::uint32_t {
    Role,
    Content,
    ToolName,
    ToolArguments,
    FinishReason,
};

const json::JsonPathProgram &openai_output_program(bool streaming) {
    static const json::JsonPathProgram buffered = [] {
        constexpr json::JsonPathRule rules[] = {
                {.expression = "$.choices[*choice].message.role",
                 .action = static_cast<std::uint32_t>(OutputAuditAction::Role)},
                {.expression = "$.choices[*choice].message.content",
                 .action = static_cast<std::uint32_t>(OutputAuditAction::Content)},
                {.expression = "$.choices[*choice].message.refusal",
                 .action = static_cast<std::uint32_t>(OutputAuditAction::Content)},
                {.expression = "$.choices[*choice].message.tool_calls[*tool].function.name",
                 .action = static_cast<std::uint32_t>(OutputAuditAction::ToolName)},
                {.expression = "$.choices[*choice].message.tool_calls[*tool].function.arguments",
                 .action = static_cast<std::uint32_t>(OutputAuditAction::ToolArguments)},
                {.expression = "$.choices[*choice].finish_reason",
                 .action = static_cast<std::uint32_t>(OutputAuditAction::FinishReason)},
        };
        auto compiled = json::JsonPathProgram::compile(rules);
        FIBER_ASSERT(compiled.has_value());
        return std::move(*compiled);
    }();
    static const json::JsonPathProgram events = [] {
        constexpr json::JsonPathRule rules[] = {
                {.expression = "$.choices[*choice].delta.role",
                 .action = static_cast<std::uint32_t>(OutputAuditAction::Role)},
                {.expression = "$.choices[*choice].delta.content",
                 .action = static_cast<std::uint32_t>(OutputAuditAction::Content)},
                {.expression = "$.choices[*choice].delta.refusal",
                 .action = static_cast<std::uint32_t>(OutputAuditAction::Content)},
                {.expression = "$.choices[*choice].delta.tool_calls[*tool].function.name",
                 .action = static_cast<std::uint32_t>(OutputAuditAction::ToolName)},
                {.expression = "$.choices[*choice].delta.tool_calls[*tool].function.arguments",
                 .action = static_cast<std::uint32_t>(OutputAuditAction::ToolArguments)},
                {.expression = "$.choices[*choice].finish_reason",
                 .action = static_cast<std::uint32_t>(OutputAuditAction::FinishReason)},
        };
        auto compiled = json::JsonPathProgram::compile(rules);
        FIBER_ASSERT(compiled.has_value());
        return std::move(*compiled);
    }();
    return streaming ? events : buffered;
}

const json::JsonPathProgram &anthropic_output_program(bool streaming) {
    static const json::JsonPathProgram buffered = [] {
        constexpr json::JsonPathRule rules[] = {
                {.expression = "$.role", .action = static_cast<std::uint32_t>(OutputAuditAction::Role)},
                {.expression = "$.content", .action = static_cast<std::uint32_t>(OutputAuditAction::Content)},
                {.expression = "$.stop_reason", .action = static_cast<std::uint32_t>(OutputAuditAction::FinishReason)},
        };
        auto compiled = json::JsonPathProgram::compile(rules);
        FIBER_ASSERT(compiled.has_value());
        return std::move(*compiled);
    }();
    static const json::JsonPathProgram events = [] {
        constexpr json::JsonPathRule rules[] = {
                {.expression = "$.message.role", .action = static_cast<std::uint32_t>(OutputAuditAction::Role)},
                {.expression = "$.content_block.text",
                 .action = static_cast<std::uint32_t>(OutputAuditAction::Content)},
                {.expression = "$.content_block.name",
                 .action = static_cast<std::uint32_t>(OutputAuditAction::ToolName)},
                {.expression = "$.delta.text", .action = static_cast<std::uint32_t>(OutputAuditAction::Content)},
                {.expression = "$.delta.partial_json",
                 .action = static_cast<std::uint32_t>(OutputAuditAction::ToolArguments)},
                {.expression = "$.delta.stop_reason",
                 .action = static_cast<std::uint32_t>(OutputAuditAction::FinishReason)},
        };
        auto compiled = json::JsonPathProgram::compile(rules);
        FIBER_ASSERT(compiled.has_value());
        return std::move(*compiled);
    }();
    return streaming ? events : buffered;
}

const json::JsonPathProgram &output_content_blocks_program() {
    static const json::JsonPathProgram program = [] {
        constexpr json::JsonPathRule rules[] = {
                {.expression = "$[*block].text", .action = static_cast<std::uint32_t>(OutputAuditAction::Content)},
                {.expression = "$[*block].name", .action = static_cast<std::uint32_t>(OutputAuditAction::ToolName)},
                {.expression = "$[*block].input",
                 .action = static_cast<std::uint32_t>(OutputAuditAction::ToolArguments)},
        };
        auto compiled = json::JsonPathProgram::compile(rules);
        FIBER_ASSERT(compiled.has_value());
        return std::move(*compiled);
    }();
    return program;
}

class LlmAuditOutputCapture {
public:
    explicit LlmAuditOutputCapture(std::size_t max_encoded_bytes) noexcept :
        content_(max_encoded_bytes), tool_names_(max_encoded_bytes), tool_arguments_(max_encoded_bytes),
        max_encoded_bytes_(max_encoded_bytes) {}

    void set_max_encoded_bytes(std::size_t value) noexcept { max_encoded_bytes_ = value; }

    [[nodiscard]] bool observe(LlmWireProtocol protocol, std::string_view input, bool streaming) noexcept {
        if (capture_failed_) {
            return false;
        }
        available_ = true;
        event_count_ = saturating_add(event_count_, 1);
        observed_json_bytes_ = saturating_add(observed_json_bytes_, input.size());
        if (hash_valid_) {
            hash_valid_ = SHA256_Update(&hash_, input.data(), input.size()) == 1 && SHA256_Update(&hash_, "\n", 1) == 1;
        }
        parse_pool_.reset();
        MatchContext context{
                .capture = this,
                .input = input,
        };
        const json::JsonPathProgram &program = protocol == LlmWireProtocol::OpenAiChatCompletions
                                                       ? openai_output_program(streaming)
                                                       : anthropic_output_program(streaming);
        auto visited = json::visit_json_paths(program, input, parse_pool_,
                                              json::JsonPathVisitor{
                                                      .context = &context,
                                                      .on_match = &LlmAuditOutputCapture::on_match,
                                              });
        if (!visited) {
            parse_errors_ = saturating_add(parse_errors_, 1);
            mark_failure("invalid_provider_json");
        }
        return !capture_failed_;
    }

    void complete(bool value) noexcept { complete_ = value; }

    [[nodiscard]] bool available() const noexcept { return available_; }
    [[nodiscard]] bool complete() const noexcept { return complete_; }
    [[nodiscard]] std::size_t event_count() const noexcept { return event_count_; }
    [[nodiscard]] std::size_t observed_json_bytes() const noexcept { return observed_json_bytes_; }
    [[nodiscard]] std::size_t parse_errors() const noexcept { return parse_errors_; }
    [[nodiscard]] std::string_view role() const noexcept { return role_.view(); }
    [[nodiscard]] const LlmAuditBuffer &content() const noexcept { return content_; }
    [[nodiscard]] const LlmAuditBuffer &tool_names() const noexcept { return tool_names_; }
    [[nodiscard]] const LlmAuditBuffer &tool_arguments() const noexcept { return tool_arguments_; }
    [[nodiscard]] std::string_view finish_reason() const noexcept { return finish_reason_.view(); }
    [[nodiscard]] std::size_t captured_text_bytes() const noexcept {
        return saturating_add(saturating_add(content_.size(), tool_names_.size()), tool_arguments_.size());
    }
    [[nodiscard]] std::size_t observed_text_bytes() const noexcept { return observed_text_bytes_; }
    [[nodiscard]] bool incomplete() const noexcept { return capture_failed_ || parse_errors_ != 0; }
    [[nodiscard]] bool canonical_complete() const noexcept { return complete_ && !incomplete(); }
    [[nodiscard]] std::string_view capture_error() const noexcept { return capture_error_; }

    [[nodiscard]] std::string_view hash() noexcept {
        if (!available_ || !hash_valid_) {
            return {};
        }
        SHA256_CTX copy = hash_;
        std::array<std::uint8_t, SHA256_DIGEST_LENGTH> digest{};
        if (SHA256_Final(digest.data(), &copy) != 1) {
            hash_valid_ = false;
            return {};
        }
        constexpr std::string_view prefix = "sha256:";
        constexpr std::string_view digits = "0123456789abcdef";
        std::memcpy(hash_text_.data(), prefix.data(), prefix.size());
        for (std::size_t i = 0; i < digest.size(); ++i) {
            hash_text_[prefix.size() + i * 2] = digits[digest[i] >> 4];
            hash_text_[prefix.size() + i * 2 + 1] = digits[digest[i] & 0x0f];
        }
        return {hash_text_.data(), hash_text_.size()};
    }

private:
    struct MatchContext {
        LlmAuditOutputCapture *capture = nullptr;
        std::string_view input;
    };

    void mark_failure(std::string_view error) noexcept {
        capture_failed_ = true;
        if (capture_error_.empty()) {
            capture_error_ = error;
        }
    }

    [[nodiscard]] bool append(LlmAuditBuffer &target, std::string_view value, bool separator = false) noexcept {
        observed_text_bytes_ = saturating_add(observed_text_bytes_, value.size());
        const std::size_t encoded = json_string_content_size(value);
        const std::size_t separator_bytes = separator ? 1 : 0;
        const std::size_t required = saturating_add(separator_bytes, encoded);
        if (encoded == std::numeric_limits<std::size_t>::max() || required > max_encoded_bytes_ ||
            captured_encoded_bytes_ > max_encoded_bytes_ - required) {
            mark_failure(encoded == std::numeric_limits<std::size_t>::max() ? "invalid_provider_utf8"
                                                                            : "audit_record_too_large");
            return false;
        }
        if ((separator && !target.append('\n')) || !target.append(value)) {
            mark_failure("audit_allocation_failed");
            return false;
        }
        captured_encoded_bytes_ += required;
        return true;
    }

    static bool on_match(void *opaque, const json::JsonPathMatch &match) noexcept {
        auto &context = *static_cast<MatchContext *>(opaque);
        LlmAuditOutputCapture &self = *context.capture;
        if (const json::JsonPathCapture *choice = match.variables.find("choice"); choice && choice->index != 0) {
            return true;
        }
        const auto action = static_cast<OutputAuditAction>(match.action);
        if (action == OutputAuditAction::Content && match.token.kind == json::TokenKind::StartArr) {
            mem::BufPool nested_pool;
            const std::string_view nested = context.input.substr(match.span.begin, match.span.size());
            MatchContext nested_context{
                    .capture = &self,
                    .input = nested,
            };
            auto visited = json::visit_json_paths(output_content_blocks_program(), nested, nested_pool,
                                                  json::JsonPathVisitor{
                                                          .context = &nested_context,
                                                          .on_match = &LlmAuditOutputCapture::on_match,
                                                  });
            if (!visited) {
                self.parse_errors_ = saturating_add(self.parse_errors_, 1);
                self.mark_failure("invalid_provider_json");
            }
            return !self.capture_failed_;
        }
        if (action == OutputAuditAction::ToolArguments &&
            (match.token.kind == json::TokenKind::StartObj || match.token.kind == json::TokenKind::StartArr)) {
            return self.append(self.tool_arguments_, context.input.substr(match.span.begin, match.span.size()),
                               !self.tool_arguments_.empty());
        }
        if (match.token.kind != json::TokenKind::Text) {
            return true;
        }
        switch (action) {
            case OutputAuditAction::Role:
                if (match.token.view.size() > 32) {
                    self.mark_failure("invalid_role");
                    return false;
                }
                self.role_.assign(match.token.view);
                break;
            case OutputAuditAction::Content:
                return self.append(self.content_, match.token.view);
            case OutputAuditAction::ToolName:
                return self.append(self.tool_names_, match.token.view, !self.tool_names_.empty());
            case OutputAuditAction::ToolArguments:
                return self.append(self.tool_arguments_, match.token.view);
            case OutputAuditAction::FinishReason:
                if (match.token.view.size() > 64) {
                    self.mark_failure("invalid_finish_reason");
                    return false;
                }
                self.finish_reason_.assign(match.token.view);
                break;
        }
        return true;
    }

    mem::BufPool parse_pool_;
    LlmAuditBuffer content_;
    LlmAuditBuffer tool_names_;
    LlmAuditBuffer tool_arguments_;
    FixedAuditText<32> role_;
    FixedAuditText<64> finish_reason_;
    SHA256_CTX hash_{};
    std::array<char, 7 + SHA256_DIGEST_LENGTH * 2> hash_text_{};
    std::size_t event_count_ = 0;
    std::size_t observed_json_bytes_ = 0;
    std::size_t observed_text_bytes_ = 0;
    std::size_t captured_encoded_bytes_ = 0;
    std::size_t parse_errors_ = 0;
    std::size_t max_encoded_bytes_ = 0;
    std::string_view capture_error_;
    bool hash_valid_ = SHA256_Init(&hash_) == 1;
    bool available_ = false;
    bool complete_ = false;
    bool capture_failed_ = false;
};

struct ProviderAttemptAudit {
    std::string_view provider;
    std::string_view token_name;
    std::string_view upstream_model;
    std::string_view path;
    std::string_view outcome;
    std::size_t index = 0;
    std::size_t total = 0;
    std::int64_t latency_us = 0;
    std::int32_t provider_config_version = 0;
    int status = 0;
    bool fallback = false;
    bool retryable = false;
    bool response_started = false;
};

class LlmRequestAudit {
public:
    LlmRequestAudit(http::HttpExchange &exchange, LlmWireProtocol protocol, AiServerCatRequest *cat_request,
                    AiServerMetrics::Worker &metrics, std::size_t max_record_bytes) noexcept :
        exchange_(&exchange), protocol_(protocol), started_(event::EventLoop::current().now()),
        started_at_ms_(wall_now_millis()), metrics_(&metrics), max_record_bytes_(max_record_bytes),
        output_(max_record_bytes),
        audit_enabled_(max_record_bytes != 0 && LOG_LLM_AUDIT.get().enabled(log::LogLevel::Info)) {
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
        if (cat_request) {
            const std::string_view cat_request_id = cat_request->request_id();
            if (!cat_request_id.empty()) {
                const std::size_t size = std::min<std::size_t>(cat_request_id.size(), request_id_storage_.size() - 1);
                std::memcpy(request_id_storage_.data(), cat_request_id.data(), size);
                request_id_ = std::string_view(request_id_storage_.data(), size);
            }
            cat_transaction_ = cat_request->root_transaction();
            if (cat_transaction_) {
                (void) cat_transaction_->add_data("protocol", protocol_name(protocol_));
            }
        }
    }

    ~LlmRequestAudit() {
        const http::HttpResponseStats &response = exchange_->response_stats();
        emit(response);
    }

    void pin_config(std::shared_ptr<const LlmConfigSnapshot> config) noexcept { config_ = std::move(config); }

    void auth_allowed(const Bt1Principal &principal) noexcept {
        auth_result_ = "allow";
        auth_reason_ = -1;
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
        auth_reason_name_ = auth_failure_reason_name(error.reason);
    }

    void request_body(const mem::IoBuf &body) noexcept {
        if (!audit_enabled_) {
            return;
        }
        body_size_ = body ? body.readable() : 0;
        const std::string_view body_view =
                body ? std::string_view(reinterpret_cast<const char *>(body.readable_data()), body.readable())
                     : std::string_view{};
        body_is_utf8_ = is_valid_utf8(body_view);
        const std::size_t body_encoded_size =
                body_is_utf8_ ? json_string_content_size(body_view) : saturating_add(body_size_, 2) / 3 * 4;
        const std::size_t required = saturating_add(body_encoded_size, kAuditRecordMetadataReserveBytes);
        if (required > max_record_bytes_) {
            capture_error_ = "audit_record_too_large";
            generation_disabled_ = true;
            return;
        }
        request_body_ = body;
        assign_sha256(body_view, body_hash_);
        output_.set_max_encoded_bytes(max_record_bytes_ - required);
    }

    void input(const LlmRoutingData &routing) noexcept {
        stream_ = routing.stream.is_present() && *routing.stream;
        requested_model_ = routing.model.is_present() ? *routing.model : std::string_view{};
        messages_count_ = routing.messages_count;
        tools_count_ = routing.tools_count;
        if (cat_transaction_ && cat_transaction_->valid()) {
            (void) cat_transaction_->add_data("stream", stream_ ? std::string_view("true") : std::string_view("false"));
        }
    }

    void output(std::string_view json, bool streaming) noexcept {
        if (audit_enabled_ && !generation_disabled_) {
            (void) output_.observe(protocol_, json, streaming);
        }
    }

    void output_complete(bool complete) noexcept { output_.complete(complete); }

    void reserve_provider_attempts(std::size_t capacity) noexcept {
        if (!audit_enabled_ || generation_disabled_ || capacity == 0) {
            return;
        }
        attempts_ = exchange_->pool().alloc<ProviderAttemptAudit>(capacity);
        if (!attempts_) {
            capture_error_ = "audit_allocation_failed";
            return;
        }
        for (std::size_t i = 0; i < capacity; ++i) {
            std::construct_at(attempts_ + i);
        }
        attempts_capacity_ = capacity;
    }

    void model(std::string_view requested, std::string_view resolved) noexcept {
        (void) requested;
        model_ = resolved;
        authz_result_ = "allow";
        if (cat_transaction_ && cat_transaction_->valid()) {
            (void) cat_transaction_->add_data("model", resolved);
        }
    }

    void authz_denied(std::string_view requested) noexcept {
        (void) requested;
        authz_result_ = "deny";
    }

    void rate_limit(std::string_view result, const TokenRateLimitCheckResult &limit) noexcept {
        rate_limit_result_ = result;
        rate_used_ = limit.used_tokens;
        rate_max_ = limit.max_tokens;
        rate_recover_at_ = limit.recover_at_millis;
    }

    void rate_limit_error() noexcept { rate_limit_result_ = "error"; }

    void usage(const std::optional<LlmTokenUsage> &usage, const ResolvedProviderAttempt &attempt) noexcept {
        if (!usage) {
            return;
        }
        usage_.merge(*usage);
        if (cat_usage_emitted_ || !usage_.has_usage_fields() || !cat_transaction_ || !cat_transaction_->valid()) {
            return;
        }
        auto event = cat_transaction_->start_event("LLMTokenUsage", attempt.protocol->model);
        if (!event) {
            return;
        }
        (void) event->add_data("provider", protocol_name(protocol_));
        (void) event->add_data("model", attempt.protocol->model);
        (void) event->add_data("resolved_provider", attempt.provider->name);
        add_cat_integer(*event, "in_cache", usage_.in_cache);
        add_cat_integer(*event, "in_nocache", usage_.in_nocache);
        add_cat_integer(*event, "out", usage_.out);
        (void) event->complete(cat::status::Success);
        cat_usage_emitted_ = true;
    }

    void provider_attempt(const ResolvedProviderAttempt &attempt, std::size_t index, std::size_t total, int status,
                          std::chrono::microseconds duration, const ProviderHttpTiming &timing, bool retryable,
                          bool response_started, std::string_view outcome) noexcept {
        if (audit_enabled_ && !generation_disabled_) {
            ++attempts_observed_;
            if (attempts_size_ < attempts_capacity_) {
                ProviderAttemptAudit &record = attempts_[attempts_size_++];
                record.provider = attempt.provider->name;
                record.token_name = attempt.api_token ? std::string_view(attempt.api_token->name) : std::string_view{};
                record.upstream_model = attempt.protocol->model;
                record.path = attempt.protocol->path;
                record.outcome = outcome;
                record.index = index + 1;
                record.total = total;
                record.provider_config_version = attempt.provider->config->metadata.version;
                record.status = status;
                record.latency_us = std::max<std::int64_t>(duration.count(), 0);
                record.fallback = attempt.fallback;
                record.retryable = retryable;
                record.response_started = response_started;
            } else {
                capture_error_ = "audit_attempt_overflow";
            }
        }
        if (cat_transaction_ && cat_transaction_->valid()) {
            std::string data;
            data.reserve(attempt.protocol->model.size() + attempt.protocol->path.size() +
                         (attempt.api_token ? attempt.api_token->name.size() : 0) + 256);
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
            if (timing.response_header_observed) {
                append_cat_duration(data, "time_to_response_header_us", timing.time_to_response_header);
            }
            if (timing.first_token_observed) {
                append_cat_duration(data, "time_to_first_token_us", timing.time_to_first_token);
            }
            if (timing.body_transfer_observed) {
                append_cat_duration(data, "body_transfer_us", timing.body_transfer);
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
    [[nodiscard]] bool encode(AuditJsonWriter &json, const http::HttpResponseStats &response,
                              std::int64_t duration_us) noexcept {
        const std::string_view output_error = output_.capture_error();
        const std::string_view capture_error = !capture_error_.empty() ? capture_error_ : output_error;
        const bool capture_complete =
                capture_error.empty() && attempts_observed_ == attempts_size_ && !output_.incomplete();
        capture_incomplete_ = !capture_complete;

        json.object_open();
        json.field("schema_version", 3);
        json.field("event", "llm_request");

        json.key("audit");
        json.object_open();
        json.field("id", request_id_);
        json.field("capture_complete", capture_complete);
        if (capture_error.empty()) {
            json.null_field("capture_error");
        } else {
            json.field("capture_error", capture_error);
        }
        json.object_close();

        json.key("request");
        json.object_open();
        json.field("started_at_ms", started_at_ms_);
        json.field("protocol", protocol_name(protocol_));
        const std::string remote_addr = exchange_->remote_addr().to_string();
        json.field("remote_addr", remote_addr);
        json.field("method", exchange_->method_view());
        json.field("path", exchange_->uri().path);
        json.field("request_model_name", requested_model_);
        json.field("stream", stream_);
        json.field("messages_count", static_cast<std::int64_t>(messages_count_));
        json.field("tools_count", static_cast<std::int64_t>(tools_count_));
        json.field("body_encoding", body_is_utf8_ ? "json_text" : "base64");
        json.key("body");
        const std::string_view body =
                request_body_ ? std::string_view(reinterpret_cast<const char *>(request_body_.readable_data()),
                                                 request_body_.readable())
                              : std::string_view{};
        if (body_is_utf8_) {
            json.text(body);
        } else {
            json.binary_text(body);
        }
        json.field("body_bytes", static_cast<std::int64_t>(body_size_));
        json.field("body_sha256", body_hash_.view());
        json.object_close();

        json.key("identity");
        json.object_open();
        json.field("auth", auth_result_);
        if (auth_reason_ < 0) {
            json.null_field("auth_reason");
            json.null_field("auth_reason_code");
        } else {
            json.field("auth_reason", auth_reason_name_);
            json.field("auth_reason_code", auth_reason_);
        }
        json.field("user", user_.view());
        json.field("kid", kid_.view());
        json.object_close();

        json.key("routing");
        json.object_open();
        json.field("resolved_model_name", model_);
        json.field("authorization", authz_result_);
        json.object_close();

        json.key("rate_limit");
        json.object_open();
        json.field("result", rate_limit_result_);
        json.field("used_tokens", rate_used_);
        json.field("max_tokens", rate_max_);
        json.field("recover_at_ms", rate_recover_at_);
        json.object_close();

        json.key("llm");
        json.object_open();
        json.key("output");
        json.object_open();
        json.field("available", output_.available());
        json.field("capture_scope", "provider_observed");
        json.field("role", output_.role());
        json.field("content", output_.content());
        json.field("tool_names", output_.tool_names());
        json.field("tool_arguments", output_.tool_arguments());
        json.field("finish_reason", output_.finish_reason());
        json.field("complete", output_.complete());
        json.field("canonical_complete", output_.canonical_complete());
        json.field("events", static_cast<std::int64_t>(output_.event_count()));
        json.field("observed_json_bytes", static_cast<std::int64_t>(output_.observed_json_bytes()));
        json.field("observed_text_bytes", static_cast<std::int64_t>(output_.observed_text_bytes()));
        json.field("captured_text_bytes", static_cast<std::int64_t>(output_.captured_text_bytes()));
        json.field("parse_errors", static_cast<std::int64_t>(output_.parse_errors()));
        const std::string_view output_hash = output_.hash();
        if (output_hash.empty()) {
            json.null_field("sha256");
        } else {
            json.field("sha256", output_hash);
        }
        json.object_close();
        json.object_close();

        json.key("provider_attempts");
        json.array_open();
        for (std::size_t i = 0; i < attempts_size_; ++i) {
            const ProviderAttemptAudit &attempt = attempts_[i];
            json.object_open();
            json.field("attempt", static_cast<std::int64_t>(attempt.index));
            json.field("total_attempts", static_cast<std::int64_t>(attempt.total));
            json.field("provider", attempt.provider);
            json.field("token_name", attempt.token_name);
            json.field("protocol", protocol_name(protocol_));
            json.field("upstream_model", attempt.upstream_model);
            json.field("path", attempt.path);
            json.field("provider_config_version", attempt.provider_config_version);
            json.field("fallback", attempt.fallback);
            json.field("status", attempt.status);
            json.field("latency_us", attempt.latency_us);
            json.field("retryable", attempt.retryable);
            json.field("response_started", attempt.response_started);
            json.field("outcome", attempt.outcome);
            json.object_close();
        }
        json.array_close();
        json.field("provider_attempt_count", static_cast<std::int64_t>(attempts_observed_));

        json.key("response");
        json.object_open();
        json.field("status", response.status_code);
        json.field("body_bytes", static_cast<std::int64_t>(response.body_bytes_sent));
        json.field("header_sent", response.header_sent);
        json.field("completed", response.completed);
        json.field("terminal_error", common::io_err_name(response.terminal_error));
        json.object_close();

        json.key("usage");
        json.object_open();
        json.field("provider", protocol_name(protocol_));
        json.optional_field("in_cache", usage_.in_cache);
        json.optional_field("in_nocache", usage_.in_nocache);
        json.optional_field("out", usage_.out);
        json.optional_field("total_tokens", usage_.total_tokens);
        json.object_close();

        json.field("duration_us", duration_us);
        json.object_close();
        return json.good();
    }

    void emit(const http::HttpResponseStats &response) noexcept {
        if (emitted_ || !audit_enabled_) {
            return;
        }
        emitted_ = true;
        if (generation_disabled_) {
            metrics_->audit_generation_failed();
            return;
        }
        const auto duration =
                std::chrono::duration_cast<std::chrono::microseconds>(event::EventLoop::current().now() - started_);
        log::LogLine line(LOG_LLM_AUDIT.get(), log::LogLevel::Info, __FILE__, __LINE__, __func__);
        if (!line.append_raw("audit_json=")) {
            line.discard();
            metrics_->audit_generation_failed();
            return;
        }
        AuditLogSink sink(line, max_record_bytes_);
        AuditJsonWriter json(sink);
        if (!encode(json, response, std::max<std::int64_t>(duration.count(), 0)) || !line.good()) {
            line.discard();
            metrics_->audit_generation_failed();
            return;
        }
        metrics_->audit_generated();
        if (capture_incomplete_) {
            metrics_->audit_capture_incomplete();
        }
    }

    http::HttpExchange *exchange_ = nullptr;
    LlmWireProtocol protocol_ = LlmWireProtocol::OpenAiChatCompletions;
    std::chrono::steady_clock::time_point started_;
    std::int64_t started_at_ms_ = 0;
    std::array<char, 1024> request_id_storage_{};
    std::string_view request_id_;
    FixedAuditText<kBt1MaxUsernameBytes> user_;
    FixedAuditText<kBt1MaxKidLength> kid_;
    std::string_view requested_model_;
    std::string_view model_;
    FixedAuditText<7 + SHA256_DIGEST_LENGTH * 2> body_hash_;
    mem::IoBuf request_body_;
    std::shared_ptr<const LlmConfigSnapshot> config_;
    std::string_view auth_result_ = "unknown";
    std::string_view auth_reason_name_;
    std::string_view authz_result_ = "unknown";
    std::string_view rate_limit_result_ = "unknown";
    std::string_view capture_error_;
    int auth_reason_ = -1;
    std::size_t body_size_ = 0;
    bool stream_ = false;
    bool body_is_utf8_ = true;
    std::size_t messages_count_ = 0;
    std::size_t tools_count_ = 0;
    ProviderAttemptAudit *attempts_ = nullptr;
    std::size_t attempts_capacity_ = 0;
    std::size_t attempts_size_ = 0;
    std::size_t attempts_observed_ = 0;
    std::int64_t rate_used_ = 0;
    std::int64_t rate_max_ = 0;
    std::int64_t rate_recover_at_ = 0;
    LlmTokenUsage usage_;
    AiServerMetrics::Worker *metrics_ = nullptr;
    std::size_t max_record_bytes_ = 0;
    LlmAuditOutputCapture output_;
    cat::Transaction *cat_transaction_ = nullptr;
    bool cat_usage_emitted_ = false;
    bool emitted_ = false;
    bool audit_enabled_ = false;
    bool generation_disabled_ = false;
    bool capture_incomplete_ = false;
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

async::Task<void> send_body(http::HttpExchange &exchange, AiServerCatRequest *cat_request, int status_code,
                            std::string_view content_type, const mem::IoBuf &body,
                            std::string_view request_id = {}) noexcept {
    http::HttpHeaders headers(exchange.pool());
    headers.set("Content-Type", content_type.empty() ? std::string_view("application/json") : content_type);
    if (!request_id.empty()) {
        headers.set("X-Request-Id", request_id);
    }
    if (cat_request) {
        cat_request->inject_response_header(headers);
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

async::Task<void> send_error(http::HttpExchange &exchange, AiServerCatRequest *cat_request, LlmWireProtocol protocol,
                             const LlmError &error, bool allow_post = false) noexcept {
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
        co_await send_body(exchange, cat_request, 500, "application/json", body);
        co_return;
    }

    http::HttpHeaders headers(exchange.pool());
    headers.set_view("Content-Type", "application/json");
    if (allow_post) {
        headers.set_view("Allow", "POST");
    }
    if (cat_request) {
        cat_request->inject_response_header(headers);
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

class ServiceInstanceRetryState {
public:
    [[nodiscard]] bool init(ServiceInstancePolicy policy, std::string_view route_key, std::size_t maximum_attempts,
                            mem::BufPool &pool) noexcept {
        policy_ = policy;
        route_key_ = route_key;
        capacity_ = maximum_attempts;
        if (policy_ == ServiceInstancePolicy::WeightedRendezvous && capacity_ > 1) {
            excluded_peer_ids_ = pool.alloc<std::uint64_t>(capacity_);
            return excluded_peer_ids_ != nullptr;
        }
        return true;
    }

    [[nodiscard]] ProviderServiceSelection selection(const ResolvedProviderAttempt &attempt) noexcept {
        if (policy_ != ServiceInstancePolicy::WeightedRendezvous) {
            return {};
        }
        if (provider_ != attempt.provider) {
            provider_ = attempt.provider;
            excluded_size_ = 0;
            rendezvous_key_ =
                    attempt.provider ? rendezvous_score(route_key_, attempt.provider->name) : std::uint64_t{0};
        }
        return ProviderServiceSelection{
                .policy = policy_,
                .rendezvous_key = rendezvous_key_,
                .excluded_peer_ids = std::span(excluded_peer_ids_, excluded_size_),
        };
    }

    void exclude(std::uint64_t peer_id) noexcept {
        if (policy_ != ServiceInstancePolicy::WeightedRendezvous || peer_id == 0 || !excluded_peer_ids_) {
            return;
        }
        if (std::find(excluded_peer_ids_, excluded_peer_ids_ + excluded_size_, peer_id) !=
            excluded_peer_ids_ + excluded_size_) {
            return;
        }
        FIBER_ASSERT(excluded_size_ < capacity_);
        excluded_peer_ids_[excluded_size_++] = peer_id;
    }

private:
    ServiceInstancePolicy policy_ = ServiceInstancePolicy::SmoothWeightedRoundRobin;
    const ProjectProvider *provider_ = nullptr;
    std::string_view route_key_;
    std::uint64_t rendezvous_key_ = 0;
    std::uint64_t *excluded_peer_ids_ = nullptr;
    std::size_t excluded_size_ = 0;
    std::size_t capacity_ = 0;
};

class RateLimitSession {
public:
    RateLimitSession() noexcept = default;
    RateLimitSession(TokenRateLimitCoordinator &manager, RateLimitNode owner, std::string_view user,
                     std::string_view model, TokenRateLimitTicket ticket, AiServerMetrics::Worker &metrics,
                     AiServerCatRequest *cat_request) noexcept :
        manager_(&manager), owner_(std::move(owner)), user_(user), model_(model), ticket_(ticket), metrics_(&metrics),
        cat_request_(cat_request) {}
    ~RateLimitSession() { settle_async(std::nullopt); }

    RateLimitSession(const RateLimitSession &) = delete;
    RateLimitSession &operator=(const RateLimitSession &) = delete;
    RateLimitSession(RateLimitSession &&other) noexcept :
        manager_(other.manager_), owner_(std::move(other.owner_)), user_(other.user_), model_(other.model_),
        ticket_(other.ticket_), metrics_(other.metrics_), cat_request_(other.cat_request_) {
        other.manager_ = nullptr;
        other.metrics_ = nullptr;
        other.cat_request_ = nullptr;
    }
    RateLimitSession &operator=(RateLimitSession &&other) noexcept {
        if (this == &other) {
            return *this;
        }
        settle_async(std::nullopt);
        manager_ = other.manager_;
        owner_ = std::move(other.owner_);
        user_ = other.user_;
        model_ = other.model_;
        ticket_ = other.ticket_;
        metrics_ = other.metrics_;
        cat_request_ = other.cat_request_;
        other.manager_ = nullptr;
        other.metrics_ = nullptr;
        other.cat_request_ = nullptr;
        return *this;
    }

    void settle_async(std::optional<std::int64_t> total_tokens) noexcept {
        if (!manager_) {
            return;
        }
        TokenRateLimitCoordinator *manager = manager_;
        AiServerMetrics::Worker *metrics = metrics_;
        manager_ = nullptr;
        metrics_ = nullptr;
        manager->settle(
                std::move(owner_), user_, model_, ticket_, total_tokens.value_or(0), total_tokens.has_value(),
                wall_now_millis(),
                RateLimitSettleCompletion{
                        .context = metrics,
                        .callback = total_tokens.has_value() ? settle_usage_completed : settle_no_usage_completed,
                },
                cat_request_);
        cat_request_ = nullptr;
    }

private:
    static void settle_usage_completed(void *context, RateLimitSettleOutcome outcome) noexcept {
        auto *metrics = static_cast<AiServerMetrics::Worker *>(context);
        metrics->rate_limit_settle(outcome == RateLimitSettleOutcome::Applied ? RateLimitSettleMetric::Usage
                                                                              : RateLimitSettleMetric::Error);
    }

    static void settle_no_usage_completed(void *context, RateLimitSettleOutcome outcome) noexcept {
        auto *metrics = static_cast<AiServerMetrics::Worker *>(context);
        metrics->rate_limit_settle(outcome == RateLimitSettleOutcome::Applied ? RateLimitSettleMetric::NoUsage
                                                                              : RateLimitSettleMetric::Error);
    }

    TokenRateLimitCoordinator *manager_ = nullptr;
    RateLimitNode owner_;
    std::string_view user_;
    std::string_view model_;
    TokenRateLimitTicket ticket_;
    AiServerMetrics::Worker *metrics_ = nullptr;
    AiServerCatRequest *cat_request_ = nullptr;
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
            const std::uint64_t failed_service_peer_id = upstream.service_peer_id();
            const ProviderHttpTiming timing = upstream.timing();
            upstream.report_instance(InstanceReportOutcome::Failure);
            co_return std::unexpected(ProviderHttpError{
                    .code = ProviderHttpErrorCode::ReadBody,
                    .io_error = chunk.error(),
                    .message = "failed to read provider response body",
                    .failed_service_peer_id = failed_service_peer_id,
                    .timing = timing,
            });
        }
        const bool complete = chunk->complete();
        const std::size_t current = response.body ? response.body.readable() : 0;
        if (chunk->readable_bytes() > maximum || current > maximum - chunk->readable_bytes()) {
            const ProviderHttpTiming timing = upstream.timing();
            (void) upstream.abort(common::IoErr::MessageTooLarge);
            upstream.report_instance(InstanceReportOutcome::Neutral);
            co_return std::unexpected(ProviderHttpError{
                    .code = ProviderHttpErrorCode::ResponseTooLarge,
                    .io_error = common::IoErr::MessageTooLarge,
                    .message = "provider response body is too large",
                    .timing = timing,
            });
        }
        if (!append_chain(response.body, *chunk, maximum)) {
            const ProviderHttpTiming timing = upstream.timing();
            (void) upstream.abort(common::IoErr::NoMem);
            upstream.report_instance(InstanceReportOutcome::Neutral);
            co_return std::unexpected(ProviderHttpError{
                    .code = ProviderHttpErrorCode::ReadBody,
                    .io_error = common::IoErr::NoMem,
                    .message = "failed to buffer provider response body",
                    .timing = timing,
            });
        }
        if (complete) {
            break;
        }
    }
    response.timing = upstream.timing();
    response.load_balance = upstream.take_load_balance();
    co_return std::move(response);
}

async::Task<bool> send_sse_header(http::HttpExchange &exchange, AiServerCatRequest *cat_request,
                                  int status_code) noexcept {
    http::HttpHeaders headers(exchange.pool());
    headers.set_view("Content-Type", "text/event-stream; charset=utf-8");
    headers.set_view("Cache-Control", "no-cache");
    headers.set_view("X-Accel-Buffering", "no");
    if (cat_request) {
        cat_request->inject_response_header(headers);
    }
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

struct SseRelayResult {
    common::IoErr upstream_error = common::IoErr::None;
    bool upstream_complete = false;
    bool downstream_delivery_failed = false;
};

common::IoErr sse_parser_io_error(SseParseError error) noexcept {
    switch (error) {
        case SseParseError::DataTooLarge:
            return common::IoErr::MessageTooLarge;
        case SseParseError::NoMemory:
            return common::IoErr::NoMem;
        case SseParseError::InvalidState:
            return common::IoErr::Invalid;
    }
    return common::IoErr::Invalid;
}

async::Task<SseRelayResult> relay_sse(http::HttpExchange &exchange, ProviderHttpResponseStream &upstream,
                                      LlmWireProtocol protocol, bool downstream_delivery_open,
                                      std::optional<LlmTokenUsage> &usage, LlmRequestAudit &audit) noexcept {
    SseParser parser;
    mem::BufPool usage_pool;
    bool downstream_delivery_failed = !downstream_delivery_open;
    auto drain_parser = [&]() noexcept -> SseParseStatus {
        for (;;) {
            const SseParseStatus status = parser.next();
            if (status != SseParseStatus::Event) {
                return status;
            }
            const std::string_view data = parser.event().data;
            if (data.empty() ||
                (protocol == LlmWireProtocol::OpenAiChatCompletions && data == std::string_view("[DONE]"))) {
                continue;
            }
            usage_pool.reset();
            LlmStreamEventObservation observation = analyze_stream_event(protocol, data, usage_pool);
            if (observation.output_token_observed) {
                upstream.observe_first_token();
            }
            audit.output(data, true);
            if (!observation.usage) {
                continue;
            }
            if (usage) {
                usage->merge(*observation.usage);
            } else {
                usage = *observation.usage;
            }
        }
    };

    for (;;) {
        auto chunk = co_await upstream.read_body(kBodyChunkBytes, kProviderTimeout);
        if (!chunk) {
            if (downstream_delivery_open && exchange.response_channel_closed()) {
                downstream_delivery_open = false;
                downstream_delivery_failed = true;
            }
            if (downstream_delivery_open) {
                (void) exchange.abort(chunk.error());
            }
            co_return SseRelayResult{
                    .upstream_error = chunk.error(),
                    .downstream_delivery_failed = downstream_delivery_failed,
            };
        }
        if (downstream_delivery_open && exchange.response_channel_closed()) {
            downstream_delivery_open = false;
            downstream_delivery_failed = true;
        }
        const bool complete = chunk->complete();
        const std::size_t expected_bytes = chunk->readable_bytes();
        mem::IoBufChain relay_chunk(chunk->node_pool());
        while (mem::IoBufNode *node = chunk->pop_front_node()) {
            const bool appended = relay_chunk.append_node(node);
            FIBER_ASSERT(appended);
            if (node->buf.readable() == 0) {
                continue;
            }
            if (!parser.feed(node->buf)) {
                const common::IoErr error = sse_parser_io_error(parser.error());
                (void) upstream.abort(error);
                if (downstream_delivery_open) {
                    (void) exchange.abort(error);
                }
                co_return SseRelayResult{
                        .upstream_error = error,
                        .downstream_delivery_failed = downstream_delivery_failed,
                };
            }
            const SseParseStatus status = drain_parser();
            if (status != SseParseStatus::NeedMore) {
                const common::IoErr error =
                        status == SseParseStatus::Error ? sse_parser_io_error(parser.error()) : common::IoErr::Invalid;
                (void) upstream.abort(error);
                if (downstream_delivery_open) {
                    (void) exchange.abort(error);
                }
                co_return SseRelayResult{
                        .upstream_error = error,
                        .downstream_delivery_failed = downstream_delivery_failed,
                };
            }
        }

        if (complete) {
            if (!parser.finish()) {
                const common::IoErr error = sse_parser_io_error(parser.error());
                (void) upstream.abort(error);
                if (downstream_delivery_open) {
                    (void) exchange.abort(error);
                }
                co_return SseRelayResult{
                        .upstream_error = error,
                        .downstream_delivery_failed = downstream_delivery_failed,
                };
            }
            const SseParseStatus status = drain_parser();
            if (status != SseParseStatus::Complete) {
                const common::IoErr error =
                        status == SseParseStatus::Error ? sse_parser_io_error(parser.error()) : common::IoErr::Invalid;
                (void) upstream.abort(error);
                if (downstream_delivery_open) {
                    (void) exchange.abort(error);
                }
                co_return SseRelayResult{
                        .upstream_error = error,
                        .downstream_delivery_failed = downstream_delivery_failed,
                };
            }
            if (downstream_delivery_open) {
                relay_chunk.mark_complete();
            }
        }

        if (!downstream_delivery_open) {
            if (complete) {
                co_return SseRelayResult{
                        .upstream_complete = true,
                        .downstream_delivery_failed = downstream_delivery_failed,
                };
            }
            continue;
        }

        if (expected_bytes == 0 && !complete) {
            continue;
        }
        auto written = co_await exchange.write_body(std::move(relay_chunk));
        if (!written || *written != expected_bytes) {
            (void) exchange.abort(written ? common::IoErr::Invalid : written.error());
            downstream_delivery_open = false;
            downstream_delivery_failed = true;
        }
        if (complete) {
            co_return SseRelayResult{
                    .upstream_complete = true,
                    .downstream_delivery_failed = downstream_delivery_failed,
            };
        }
    }
}

bool should_retry_provider(const http::HttpExchange &exchange, const ProviderErrorDecision &decision,
                           bool response_started, std::size_t attempt_index, std::size_t attempt_count) noexcept {
    return decision.retryable && !response_started && attempt_index + 1 < attempt_count &&
           !exchange.response_channel_closed();
}

} // namespace

async::Task<void> LlmRequestHandler::handle(http::HttpExchange &exchange, LlmWireProtocol protocol,
                                            std::shared_ptr<const LlmConfigSnapshot> config,
                                            AiServerCatRequest *cat_request) noexcept {
    LlmRequestAudit audit(exchange, protocol, cat_request, *metrics_, audit_max_record_bytes_);
    audit.pin_config(config);
    auto authenticated = authenticate_llm_request(exchange.request_headers(), std::move(config), wall_now_seconds());
    if (!authenticated) {
        audit.auth_denied(authenticated.error());
        co_await send_error(exchange, cat_request, protocol, auth_error(authenticated.error()));
        co_return;
    }
    audit.auth_allowed(authenticated->principal());
    if (exchange.method() != http::HttpMethod::Post) {
        co_await send_error(exchange, cat_request, protocol,
                            input_error(405, "method_not_allowed", "method not allowed"), true);
        co_return;
    }
    const auto *content_type = exchange.content_type_header();
    if (!content_type || !is_json_content_type(content_type->value_view())) {
        co_await send_error(exchange, cat_request, protocol,
                            input_error(415, "unsupported_media_type", "content-type must be application/json"));
        co_return;
    }

    auto raw_body = co_await read_request_body(exchange);
    if (!raw_body) {
        if (raw_body.error().code == ReadRequestBodyError::TooLarge) {
            co_await send_error(exchange, cat_request, protocol,
                                input_error(413, "request_too_large", "request body is too large"));
        } else if (raw_body.error().code == ReadRequestBodyError::OutOfMemory) {
            co_await send_error(exchange, cat_request, protocol,
                                LlmError{
                                        .status_code = 500,
                                        .code = "internal_error",
                                        .type = protocol == LlmWireProtocol::OpenAiChatCompletions
                                                        ? std::string_view("server_error")
                                                        : std::string_view("api_error"),
                                        .message = "internal server error",
                                });
        } else {
            co_await send_error(exchange, cat_request, protocol,
                                input_error(400, "request_body_error", "read request body failed"));
        }
        co_return;
    }
    audit.request_body(*raw_body);

    auto parsed = ParsedLlmBody::parse(protocol, std::move(*raw_body), exchange.pool());
    if (!parsed) {
        co_await send_error(exchange, cat_request, protocol,
                            input_error(400, "invalid_json", "invalid json request body"));
        co_return;
    }
    const LlmRoutingData &routing = parsed->routing();
    audit.input(routing);
    const std::string_view requested_model = routing.model.is_present() ? *routing.model : std::string_view{};
    auto authorized = authorize_model(authenticated->config(), authenticated->principal().username(), requested_model);
    if (!authorized) {
        audit.authz_denied(requested_model);
        co_await send_error(exchange, cat_request, protocol, model_error(protocol, authorized.error()));
        co_return;
    }
    if (cat_request) {
        (void) cat_request->set_root_model_name(authorized->model_name);
    }
    audit.model(requested_model, authorized->model_name);

    auto route_key = build_provider_route_key(protocol, routing, authorized->route->load_balance, exchange.pool());
    if (!route_key) {
        co_await send_error(exchange, cat_request, protocol,
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
                                                            wall_now_millis(), cat_request);
    if (!coordinated_limit) {
        audit.rate_limit_error();
        metrics_->rate_limit_check(RateLimitCheckMetric::Error);
        co_await send_error(exchange, cat_request, protocol,
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
        co_await send_error(exchange, cat_request, protocol,
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
                                      *metrics_, cat_request);
    }

    auto plan = resolve_execution_plan(*authorized, protocol, *route_key, *provider_runtime_,
                                       event::EventLoop::current().now(), exchange.pool());
    if (!plan) {
        co_await send_error(exchange, cat_request, protocol, plan_error(protocol, plan.error()));
        co_return;
    }
    audit.reserve_provider_attempts(plan->attempts.size());

    ServiceInstanceRetryState service_instances;
    if (!service_instances.init(plan->load_balance.service_instance_policy, plan->route_key, plan->attempts.size(),
                                exchange.pool())) {
        co_await send_error(exchange, cat_request, protocol,
                            plan_error(protocol, ExecutionPlanError{
                                                         .code = ExecutionPlanErrorCode::OutOfMemory,
                                                         .message = "failed to allocate service retry state",
                                                 }));
        co_return;
    }

    const bool stream = routing.stream.is_present() && *routing.stream;
    for (std::size_t index = 0; index < plan->attempts.size(); ++index) {
        if (exchange.response_channel_closed()) {
            co_return;
        }
        const ResolvedProviderAttempt &attempt = plan->attempts[index];
        const ProviderServiceSelection service_selection = service_instances.selection(attempt);
        auto rewritten =
                parsed->rewrite(attempt.protocol->model,
                                routing.stream.is_present() ? std::optional<bool>(*routing.stream) : std::nullopt,
                                event::EventLoop::current().io_buf_node_pool());
        if (!rewritten) {
            co_await send_error(exchange, cat_request, protocol,
                                input_error(400, "invalid_json", "failed to modify request body"));
            co_return;
        }

        std::optional<cat::PropagationContext> provider_cat_context;
        if (cat_request) {
            auto remote_context = cat_request->create_remote_context();
            if (remote_context) {
                provider_cat_context.emplace(std::move(*remote_context));
            }
        }

        if (stream) {
            metrics_->provider_attempt(protocol);
            const auto attempt_started = event::EventLoop::current().now();
            auto started = co_await provider_client_->start(
                    attempt, true, std::move(*rewritten), exchange.pool(), service_selection,
                    provider_cat_context ? &*provider_cat_context : nullptr,
                    cat_request ? cat_request->trace_state() : std::string_view{});
            if (!started) {
                service_instances.exclude(started.error().failed_service_peer_id);
                metrics_->provider_failure(protocol);
                const ProviderErrorDecision decision = classify_provider_transport_error(false);
                apply_observed_provider_error(attempt, decision, event::EventLoop::current().now(), *metrics_,
                                              protocol);
                audit.provider_attempt(attempt, index, plan->attempts.size(), 0,
                                       std::chrono::duration_cast<std::chrono::microseconds>(
                                               event::EventLoop::current().now() - attempt_started),
                                       started.error().timing, decision.retryable, false, "transport_error");
                if (should_retry_provider(exchange, decision, false, index, plan->attempts.size())) {
                    metrics_->provider_retry(protocol);
                    continue;
                }
                if (exchange.response_channel_closed()) {
                    co_return;
                }
                co_await send_error(exchange, cat_request, protocol,
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
                    service_instances.exclude(buffered.error().failed_service_peer_id);
                    metrics_->provider_failure(protocol);
                    const ProviderErrorDecision decision = classify_provider_transport_error(false);
                    apply_observed_provider_error(attempt, decision, event::EventLoop::current().now(), *metrics_,
                                                  protocol);
                    audit.provider_attempt(attempt, index, plan->attempts.size(), upstream_status,
                                           std::chrono::duration_cast<std::chrono::microseconds>(
                                                   event::EventLoop::current().now() - attempt_started),
                                           buffered.error().timing, decision.retryable, false, "response_read_error");
                    if (should_retry_provider(exchange, decision, false, index, plan->attempts.size())) {
                        metrics_->provider_retry(protocol);
                        continue;
                    }
                    if (exchange.response_channel_closed()) {
                        co_return;
                    }
                    co_await send_error(exchange, cat_request, protocol,
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
                if (decision.instance_outcome == InstanceReportOutcome::Failure) {
                    service_instances.exclude(buffered->load_balance.peer_id());
                }
                buffered->load_balance.report(decision.instance_outcome);
                apply_observed_provider_error(attempt, decision, event::EventLoop::current().now(), *metrics_,
                                              protocol);
                metrics_->provider_failure(protocol);
                audit.provider_attempt(attempt, index, plan->attempts.size(), buffered->status_code,
                                       std::chrono::duration_cast<std::chrono::microseconds>(
                                               event::EventLoop::current().now() - attempt_started),
                                       buffered->timing, decision.retryable, false, "upstream_error");
                if (should_retry_provider(exchange, decision, false, index, plan->attempts.size())) {
                    metrics_->provider_retry(protocol);
                    continue;
                }
                if (exchange.response_channel_closed()) {
                    co_return;
                }
                if (!buffered->body || buffered->body.readable() == 0) {
                    co_await send_error(exchange, cat_request, protocol,
                                        LlmError{
                                                .status_code = 502,
                                                .code = "upstream_invalid_error_response",
                                                .type = "api_error",
                                                .message = "provider error response is empty",
                                        });
                    co_return;
                }
                co_await send_body(exchange, cat_request, buffered->status_code, buffered->content_type, buffered->body,
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
                                       started->timing(), false, false, "invalid_content_type");
                (void) started->abort(common::IoErr::Invalid);
                started->report_instance(InstanceReportOutcome::Failure);
                if (exchange.response_channel_closed()) {
                    co_return;
                }
                co_await send_error(exchange, cat_request, protocol,
                                    LlmError{
                                            .status_code = 502,
                                            .code = "upstream_invalid_response",
                                            .type = "api_error",
                                            .message = "provider did not return an event stream",
                                    });
                co_return;
            }
            const bool response_started = co_await send_sse_header(exchange, cat_request, started->status_code());
            std::optional<LlmTokenUsage> usage;
            const SseRelayResult relay_result =
                    co_await relay_sse(exchange, *started, protocol, response_started, usage, audit);
            rate_limit.settle_async(usage ? usage->total_tokens : std::optional<std::int64_t>{});
            audit.output_complete(relay_result.upstream_complete);
            if (relay_result.upstream_complete) {
                started->report_instance(InstanceReportOutcome::Success);
                attempt.runtime->record_success(attempt.api_token ? attempt.api_token->name : std::string_view{});
            } else {
                started->report_instance(InstanceReportOutcome::Failure);
                metrics_->provider_failure(protocol);
                apply_observed_provider_error(attempt, classify_provider_transport_error(true),
                                              event::EventLoop::current().now(), *metrics_, protocol);
            }
            if (relay_result.downstream_delivery_failed || !relay_result.upstream_complete) {
                metrics_->sse_failure(protocol);
            }
            if (relay_result.downstream_delivery_failed) {
                const SseDrainMetric drain_result = relay_result.upstream_complete
                                                            ? SseDrainMetric::Completed
                                                            : (relay_result.upstream_error == common::IoErr::TimedOut
                                                                       ? SseDrainMetric::Timeout
                                                                       : SseDrainMetric::UpstreamError);
                metrics_->sse_drain(protocol, drain_result);
            }
            const std::string_view outcome =
                    relay_result.upstream_complete ? std::string_view("success") : std::string_view("stream_error");
            audit.provider_attempt(attempt, index, plan->attempts.size(), started->status_code(),
                                   std::chrono::duration_cast<std::chrono::microseconds>(
                                           event::EventLoop::current().now() - attempt_started),
                                   started->timing(), false, response_started, outcome);
            audit.usage(usage, attempt);
            if (usage) {
                metrics_->token_usage(authenticated->principal().username(), attempt.provider->name, protocol, *usage);
            }
            co_return;
        }

        metrics_->provider_attempt(protocol);
        const auto attempt_started = event::EventLoop::current().now();
        auto response = co_await provider_client_->execute_buffered(
                attempt, false, std::move(*rewritten), exchange.pool(), kMaxProviderResponseBytes, service_selection,
                provider_cat_context ? &*provider_cat_context : nullptr,
                cat_request ? cat_request->trace_state() : std::string_view{});
        if (!response) {
            service_instances.exclude(response.error().failed_service_peer_id);
            metrics_->provider_failure(protocol);
            const ProviderErrorDecision decision = classify_provider_transport_error(false);
            apply_observed_provider_error(attempt, decision, event::EventLoop::current().now(), *metrics_, protocol);
            audit.provider_attempt(attempt, index, plan->attempts.size(), 0,
                                   std::chrono::duration_cast<std::chrono::microseconds>(
                                           event::EventLoop::current().now() - attempt_started),
                                   response.error().timing, decision.retryable, false, "transport_error");
            if (should_retry_provider(exchange, decision, false, index, plan->attempts.size())) {
                metrics_->provider_retry(protocol);
                continue;
            }
            if (exchange.response_channel_closed()) {
                co_return;
            }
            co_await send_error(exchange, cat_request, protocol,
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
            mem::BufPool usage_pool;
            auto usage = extract_token_usage(protocol, io_buf_view(response->body), false, usage_pool);
            audit.output(io_buf_view(response->body), false);
            audit.output_complete(true);
            response->load_balance.report(InstanceReportOutcome::Success);
            attempt.runtime->record_success(attempt.api_token ? attempt.api_token->name : std::string_view{});
            audit.provider_attempt(attempt, index, plan->attempts.size(), response->status_code,
                                   std::chrono::duration_cast<std::chrono::microseconds>(
                                           event::EventLoop::current().now() - attempt_started),
                                   response->timing, false, false, "success");
            audit.usage(usage, attempt);
            if (usage) {
                metrics_->token_usage(authenticated->principal().username(), attempt.provider->name, protocol, *usage);
            }
            rate_limit.settle_async(usage ? usage->total_tokens : std::optional<std::int64_t>{});
            co_await send_body(exchange, cat_request, response->status_code, response->content_type, response->body,
                               response->request_id);
            co_return;
        }

        const ProviderErrorDecision decision =
                classify_provider_response(protocol, response->status_code, response->retry_after,
                                           io_buf_view(response->body), plan->load_balance, false, exchange.pool());
        if (decision.instance_outcome == InstanceReportOutcome::Failure) {
            service_instances.exclude(response->load_balance.peer_id());
        }
        response->load_balance.report(decision.instance_outcome);
        apply_observed_provider_error(attempt, decision, event::EventLoop::current().now(), *metrics_, protocol);
        metrics_->provider_failure(protocol);
        audit.provider_attempt(attempt, index, plan->attempts.size(), response->status_code,
                               std::chrono::duration_cast<std::chrono::microseconds>(event::EventLoop::current().now() -
                                                                                     attempt_started),
                               response->timing, decision.retryable, false, "upstream_error");
        if (should_retry_provider(exchange, decision, false, index, plan->attempts.size())) {
            metrics_->provider_retry(protocol);
            continue;
        }
        if (exchange.response_channel_closed()) {
            co_return;
        }
        if (!response->body || response->body.readable() == 0) {
            co_await send_error(exchange, cat_request, protocol,
                                LlmError{
                                        .status_code = 502,
                                        .code = "upstream_invalid_error_response",
                                        .type = "api_error",
                                        .message = "provider error response is empty",
                                });
            co_return;
        }
        co_await send_body(exchange, cat_request, response->status_code, response->content_type, response->body,
                           response->request_id);
        co_return;
    }

    co_await send_error(exchange, cat_request, protocol,
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
