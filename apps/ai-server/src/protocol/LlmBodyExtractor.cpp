#include "LlmBody.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>

#include <common/Assert.h>
#include <common/json/JsonPath.h>

namespace fiber::ai_server {
namespace {

using json::JsonPathMatch;
using json::JsonPathProgram;
using json::JsonPathRule;
using json::JsonPathVisitor;
using json::Nullable;
using json::TokenKind;

enum class BodyAction : std::uint32_t {
    Model,
    Stream,
    MetadataRouteKey,
    MetadataRouteKeyCamel,
    Container,
    PromptCacheKey,
    MessageRole,
    MessageContent,
    System,
};

enum class AuditAction : std::uint32_t {
    SystemText,
    MessageText,
    ToolName,
    ToolDescription,
    ToolArguments,
};

constexpr std::size_t kMaxAuditPromptParts = 256;
constexpr std::size_t kMaxNestedAuditPromptBytes = 24 * 1024;

template<typename T>
class RequestArrayBuilder {
    static_assert(std::is_trivially_copyable_v<T>);

public:
    explicit RequestArrayBuilder(mem::BufPool &pool) noexcept : pool_(&pool) {}

    [[nodiscard]] bool append(const T &value) noexcept {
        if (size_ == capacity_ && !grow()) {
            return false;
        }
        data_[size_++] = value;
        return true;
    }

    [[nodiscard]] json::JsonArray<T> finish() const noexcept { return json::JsonArray<T>(data_, size_); }

private:
    [[nodiscard]] bool grow() noexcept {
        const std::size_t next = capacity_ == 0 ? 8 : capacity_ * 2;
        if (next < capacity_ || next > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            return false;
        }
        T *replacement = pool_->alloc<T>(next);
        if (!replacement) {
            return false;
        }
        if (size_ > 0) {
            std::memcpy(replacement, data_, size_ * sizeof(T));
        }
        data_ = replacement;
        capacity_ = next;
        return true;
    }

    mem::BufPool *pool_ = nullptr;
    T *data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t capacity_ = 0;
};

const JsonPathProgram &openai_program() {
    static const JsonPathProgram program = [] {
        constexpr JsonPathRule rules[] = {
                {.expression = "$.model", .action = static_cast<std::uint32_t>(BodyAction::Model)},
                {.expression = "$.stream", .action = static_cast<std::uint32_t>(BodyAction::Stream)},
                {.expression = "$.metadata.route_key",
                 .action = static_cast<std::uint32_t>(BodyAction::MetadataRouteKey)},
                {.expression = "$.metadata.routeKey",
                 .action = static_cast<std::uint32_t>(BodyAction::MetadataRouteKeyCamel)},
                {.expression = "$.prompt_cache_key", .action = static_cast<std::uint32_t>(BodyAction::PromptCacheKey)},
                {.expression = "$.messages[*message].role",
                 .action = static_cast<std::uint32_t>(BodyAction::MessageRole)},
                {.expression = "$.messages[*message].content",
                 .action = static_cast<std::uint32_t>(BodyAction::MessageContent)},
        };
        auto result = JsonPathProgram::compile(rules);
        FIBER_ASSERT(result.has_value());
        return std::move(*result);
    }();
    return program;
}

const JsonPathProgram &anthropic_program() {
    static const JsonPathProgram program = [] {
        constexpr JsonPathRule rules[] = {
                {.expression = "$.model", .action = static_cast<std::uint32_t>(BodyAction::Model)},
                {.expression = "$.stream", .action = static_cast<std::uint32_t>(BodyAction::Stream)},
                {.expression = "$.metadata.route_key",
                 .action = static_cast<std::uint32_t>(BodyAction::MetadataRouteKey)},
                {.expression = "$.metadata.routeKey",
                 .action = static_cast<std::uint32_t>(BodyAction::MetadataRouteKeyCamel)},
                {.expression = "$.container", .action = static_cast<std::uint32_t>(BodyAction::Container)},
                {.expression = "$.messages[*message].role",
                 .action = static_cast<std::uint32_t>(BodyAction::MessageRole)},
                {.expression = "$.messages[*message].content",
                 .action = static_cast<std::uint32_t>(BodyAction::MessageContent)},
                {.expression = "$.system", .action = static_cast<std::uint32_t>(BodyAction::System)},
        };
        auto result = JsonPathProgram::compile(rules);
        FIBER_ASSERT(result.has_value());
        return std::move(*result);
    }();
    return program;
}

const JsonPathProgram &openai_audit_program() {
    static const JsonPathProgram program = [] {
        constexpr JsonPathRule rules[] = {
                {.expression = "$.messages[*message].content[*block].text",
                 .action = static_cast<std::uint32_t>(AuditAction::MessageText)},
                {.expression = "$.messages[*message].content[*block].input_text",
                 .action = static_cast<std::uint32_t>(AuditAction::MessageText)},
                {.expression = "$.messages[*message].tool_calls[*tool].function.name",
                 .action = static_cast<std::uint32_t>(AuditAction::ToolName)},
                {.expression = "$.messages[*message].tool_calls[*tool].function.arguments",
                 .action = static_cast<std::uint32_t>(AuditAction::ToolArguments)},
                {.expression = "$.tools[*tool].function.name",
                 .action = static_cast<std::uint32_t>(AuditAction::ToolName)},
                {.expression = "$.tools[*tool].function.description",
                 .action = static_cast<std::uint32_t>(AuditAction::ToolDescription)},
        };
        auto result = JsonPathProgram::compile(rules);
        FIBER_ASSERT(result.has_value());
        return std::move(*result);
    }();
    return program;
}

const JsonPathProgram &anthropic_audit_program() {
    static const JsonPathProgram program = [] {
        constexpr JsonPathRule rules[] = {
                {.expression = "$.system[*block].text", .action = static_cast<std::uint32_t>(AuditAction::SystemText)},
                {.expression = "$.messages[*message].content[*block].text",
                 .action = static_cast<std::uint32_t>(AuditAction::MessageText)},
                {.expression = "$.messages[*message].content[*block].content",
                 .action = static_cast<std::uint32_t>(AuditAction::MessageText)},
                {.expression = "$.messages[*message].content[*block].name",
                 .action = static_cast<std::uint32_t>(AuditAction::ToolName)},
                {.expression = "$.tools[*tool].name", .action = static_cast<std::uint32_t>(AuditAction::ToolName)},
                {.expression = "$.tools[*tool].description",
                 .action = static_cast<std::uint32_t>(AuditAction::ToolDescription)},
        };
        auto result = JsonPathProgram::compile(rules);
        FIBER_ASSERT(result.has_value());
        return std::move(*result);
    }();
    return program;
}

bool copy_text(mem::BufPool &pool, std::string_view value, std::string_view &out) noexcept {
    if (value.empty()) {
        out = {};
        return true;
    }
    auto *data = static_cast<char *>(pool.alloc(value.size(), alignof(char)));
    if (!data) {
        return false;
    }
    std::memcpy(data, value.data(), value.size());
    out = std::string_view(data, value.size());
    return true;
}

std::size_t utf8_prefix_size(std::string_view value, std::size_t maximum) noexcept {
    std::size_t size = std::min(value.size(), maximum);
    if (size == value.size()) {
        return size;
    }
    while (size > 0 && (static_cast<unsigned char>(value[size]) & 0xc0) == 0x80) {
        --size;
    }
    return size;
}

struct ExtractContext {
    explicit ExtractContext(std::string_view input, mem::BufPool &pool) noexcept :
        input(input), pool(&pool), roles(pool), contents(pool), prompt_parts(pool), patches(pool) {}

    [[nodiscard]] bool set_error(LlmBodyErrorCode code, const JsonPathMatch &match, std::string_view field,
                                 const char *message) noexcept {
        error = LlmBodyError{
                .code = code,
                .offset = match.span.begin,
                .field = field,
                .message = message,
        };
        return false;
    }

    [[nodiscard]] bool copy_match_text(const JsonPathMatch &match, std::string_view field,
                                       Nullable<std::string_view> &target) noexcept {
        if (match.token.kind == TokenKind::Null) {
            target.set_null();
            return true;
        }
        if (match.token.kind != TokenKind::Text) {
            return set_error(LlmBodyErrorCode::InvalidFieldType, match, field, "expected a JSON string");
        }
        std::string_view value;
        if (!copy_text(*pool, match.token.view, value)) {
            return set_error(LlmBodyErrorCode::OutOfMemory, match, field, "out of memory");
        }
        target.set_present(value);
        return true;
    }

    [[nodiscard]] bool read_text_or_null(const JsonPathMatch &match, Nullable<std::string_view> &target) noexcept {
        if (match.token.kind == TokenKind::Null || match.token.kind == TokenKind::StartObj ||
            match.token.kind == TokenKind::StartArr) {
            target.set_null();
            return true;
        }

        std::string_view source;
        if (match.token.kind == TokenKind::Text) {
            source = match.token.view;
        } else {
            source = input.substr(match.span.begin, match.span.size());
        }
        std::string_view value;
        if (!copy_text(*pool, source, value)) {
            return set_error(LlmBodyErrorCode::OutOfMemory, match, {}, "out of memory");
        }
        target.set_present(value);
        return true;
    }

    [[nodiscard]] bool append_prompt_part(LlmAuditPromptPartKind kind, std::size_t message_index,
                                          std::size_t item_index, std::string_view text,
                                          const JsonPathMatch &match) noexcept {
        if (routing.audit_prompt_incomplete) {
            return true;
        }
        if (prompt_parts_count == kMaxAuditPromptParts) {
            routing.audit_prompt_truncated = true;
            return true;
        }
        if (!prompt_parts.append(LlmAuditPromptPart{
                    .kind = kind,
                    .message_index = message_index,
                    .item_index = item_index,
                    .text = text,
            })) {
            routing.audit_prompt_incomplete = true;
            return true;
        }
        ++prompt_parts_count;
        return true;
    }

    [[nodiscard]] bool append_nested_prompt_part(LlmAuditPromptPartKind kind, std::size_t message_index,
                                                 std::size_t item_index, const JsonPathMatch &match) noexcept {
        if (match.token.kind != TokenKind::Text) {
            return true;
        }
        const std::size_t remaining =
                nested_prompt_bytes < kMaxNestedAuditPromptBytes ? kMaxNestedAuditPromptBytes - nested_prompt_bytes : 0;
        if (remaining == 0) {
            routing.audit_prompt_truncated = true;
            return true;
        }
        const std::size_t copy_size = utf8_prefix_size(match.token.view, remaining);
        std::string_view value;
        if (!copy_text(*pool, match.token.view.substr(0, copy_size), value)) {
            routing.audit_prompt_incomplete = true;
            return false;
        }
        nested_prompt_bytes += copy_size;
        if (copy_size != match.token.view.size()) {
            routing.audit_prompt_truncated = true;
        }
        return append_prompt_part(kind, message_index, item_index, value, match);
    }

    void observe_message_index(const JsonPathMatch &match) noexcept {
        const json::JsonPathCapture *capture = match.variables.find("message");
        if (capture && capture->kind == json::JsonPathCaptureKind::ArrayIndex &&
            capture->index != std::numeric_limits<std::size_t>::max()) {
            routing.messages_count = std::max(routing.messages_count, capture->index + 1);
        }
    }

    static bool on_match(void *opaque, const JsonPathMatch &match) noexcept {
        auto &self = *static_cast<ExtractContext *>(opaque);
        const auto action = static_cast<BodyAction>(match.action);
        if (action == BodyAction::Model || action == BodyAction::Stream) {
            if (!self.patches.append(LlmBodyPatchSite{
                        .begin = match.span.begin,
                        .end = match.span.end,
                        .kind = action == BodyAction::Model ? LlmBodyPatchKind::Model : LlmBodyPatchKind::Stream,
                })) {
                return self.set_error(LlmBodyErrorCode::OutOfMemory, match, {}, "out of memory");
            }
        }

        switch (action) {
            case BodyAction::Model:
                return self.copy_match_text(match, "$.model", self.routing.model);
            case BodyAction::Stream:
                if (match.token.kind == TokenKind::Null) {
                    self.routing.stream.set_null();
                    return true;
                }
                if (match.token.kind != TokenKind::Bool) {
                    return self.set_error(LlmBodyErrorCode::InvalidFieldType, match, "$.stream",
                                          "expected a JSON boolean");
                }
                self.routing.stream.set_present(match.token.bval);
                return true;
            case BodyAction::MetadataRouteKey:
            case BodyAction::MetadataRouteKeyCamel: {
                Nullable<std::string_view> value;
                if (!self.copy_match_text(match,
                                          action == BodyAction::MetadataRouteKey ? "$.metadata.route_key"
                                                                                 : "$.metadata.routeKey",
                                          value)) {
                    return false;
                }
                if (!value.is_present() || value->empty()) {
                    return true;
                }
                if (action == BodyAction::MetadataRouteKey || !self.routing.metadata_route_key.is_present()) {
                    self.routing.metadata_route_key = value;
                }
                return true;
            }
            case BodyAction::Container:
                return self.copy_match_text(match, "$.container", self.routing.container);
            case BodyAction::PromptCacheKey:
                return self.copy_match_text(match, "$.prompt_cache_key", self.routing.prompt_cache_key);
            case BodyAction::MessageRole: {
                self.observe_message_index(match);
                Nullable<std::string_view> value;
                if (!self.copy_match_text(match, "$.messages[*].role", value)) {
                    return false;
                }
                if (!self.roles.append(value)) {
                    return self.set_error(LlmBodyErrorCode::OutOfMemory, match, {}, "out of memory");
                }
                const json::JsonPathCapture *message = match.variables.find("message");
                if (value.is_present() && message &&
                    !self.append_prompt_part(LlmAuditPromptPartKind::MessageRole, message->index,
                                             LlmAuditPromptPart::NoIndex, *value, match)) {
                    return false;
                }
                return true;
            }
            case BodyAction::MessageContent: {
                self.observe_message_index(match);
                Nullable<std::string_view> value;
                if (!self.read_text_or_null(match, value)) {
                    return false;
                }
                if (!self.contents.append(value)) {
                    return self.set_error(LlmBodyErrorCode::OutOfMemory, match, {}, "out of memory");
                }
                const json::JsonPathCapture *message = match.variables.find("message");
                if (value.is_present() && message &&
                    !self.append_prompt_part(LlmAuditPromptPartKind::MessageText, message->index,
                                             LlmAuditPromptPart::NoIndex, *value, match)) {
                    return false;
                }
                return true;
            }
            case BodyAction::System:
                if (!self.read_text_or_null(match, self.routing.system_text)) {
                    return false;
                }
                return !self.routing.system_text.is_present() ||
                       self.append_prompt_part(LlmAuditPromptPartKind::SystemText, LlmAuditPromptPart::NoIndex,
                                               LlmAuditPromptPart::NoIndex, *self.routing.system_text, match);
        }
        return self.set_error(LlmBodyErrorCode::InvalidJson, match, {}, "unknown JSON path action");
    }

    static bool on_audit_match(void *opaque, const JsonPathMatch &match) noexcept {
        auto &self = *static_cast<ExtractContext *>(opaque);
        const json::JsonPathCapture *message = match.variables.find("message");
        const json::JsonPathCapture *block = match.variables.find("block");
        const json::JsonPathCapture *tool = match.variables.find("tool");
        if (message) {
            self.observe_message_index(match);
        }
        if (tool && tool->kind == json::JsonPathCaptureKind::ArrayIndex &&
            tool->index != std::numeric_limits<std::size_t>::max()) {
            self.routing.tools_count = std::max(self.routing.tools_count, tool->index + 1);
        }

        LlmAuditPromptPartKind kind = LlmAuditPromptPartKind::MessageText;
        switch (static_cast<AuditAction>(match.action)) {
            case AuditAction::SystemText:
                kind = LlmAuditPromptPartKind::SystemText;
                break;
            case AuditAction::MessageText:
                kind = LlmAuditPromptPartKind::MessageText;
                break;
            case AuditAction::ToolName:
                kind = LlmAuditPromptPartKind::ToolName;
                break;
            case AuditAction::ToolDescription:
                kind = LlmAuditPromptPartKind::ToolDescription;
                break;
            case AuditAction::ToolArguments:
                kind = LlmAuditPromptPartKind::ToolArguments;
                break;
        }
        return self.append_nested_prompt_part(kind, message ? message->index : LlmAuditPromptPart::NoIndex,
                                              tool ? tool->index : (block ? block->index : LlmAuditPromptPart::NoIndex),
                                              match);
    }

    std::string_view input;
    mem::BufPool *pool = nullptr;
    LlmRoutingData routing;
    RequestArrayBuilder<Nullable<std::string_view>> roles;
    RequestArrayBuilder<Nullable<std::string_view>> contents;
    RequestArrayBuilder<LlmAuditPromptPart> prompt_parts;
    RequestArrayBuilder<LlmBodyPatchSite> patches;
    std::optional<LlmBodyError> error;
    std::size_t prompt_parts_count = 0;
    std::size_t nested_prompt_bytes = 0;
};

bool root_is_object(std::string_view input) noexcept {
    for (char ch: input) {
        switch (ch) {
            case ' ':
            case '\t':
            case '\r':
            case '\n':
                continue;
            default:
                return ch == '{';
        }
    }
    return false;
}

} // namespace

std::expected<ParsedLlmBody, LlmBodyError> ParsedLlmBody::parse(LlmWireProtocol protocol, mem::IoBuf body,
                                                                mem::BufPool &pool) noexcept {
    if (!body) {
        return std::unexpected(LlmBodyError{
                .code = LlmBodyErrorCode::OutOfMemory,
                .message = "request body storage is unavailable",
        });
    }

    const std::string_view input(reinterpret_cast<const char *>(body.readable_data()), body.readable());
    ExtractContext context(input, pool);
    const JsonPathProgram &program =
            protocol == LlmWireProtocol::OpenAiChatCompletions ? openai_program() : anthropic_program();
    auto visited = json::visit_json_paths(program, input, pool,
                                          JsonPathVisitor{
                                                  .context = &context,
                                                  .on_match = &ExtractContext::on_match,
                                          });
    if (!visited) {
        if (context.error) {
            return std::unexpected(*context.error);
        }
        return std::unexpected(LlmBodyError{
                .code = visited.error().code == json::JsonPathVisitErrorCode::OutOfMemory
                                ? LlmBodyErrorCode::OutOfMemory
                                : LlmBodyErrorCode::InvalidJson,
                .offset = visited.error().parse_error.offset,
                .message = visited.error().parse_error.message ? visited.error().parse_error.message
                                                               : "failed to parse JSON request body",
        });
    }
    if (!root_is_object(input)) {
        return std::unexpected(LlmBodyError{
                .code = LlmBodyErrorCode::ExpectedObject,
                .message = "request body must be a JSON object",
        });
    }

    const JsonPathProgram &audit_program =
            protocol == LlmWireProtocol::OpenAiChatCompletions ? openai_audit_program() : anthropic_audit_program();
    auto audit_visited = json::visit_json_paths(audit_program, input, pool,
                                                JsonPathVisitor{
                                                        .context = &context,
                                                        .on_match = &ExtractContext::on_audit_match,
                                                });
    if (!audit_visited) {
        context.routing.audit_prompt_incomplete = true;
    }

    context.routing.message_roles = context.roles.finish();
    context.routing.message_content_texts = context.contents.finish();
    context.routing.audit_prompt_parts = context.prompt_parts.finish();
    context.routing.messages_count = std::max(context.routing.messages_count, context.routing.message_roles.size());
    return ParsedLlmBody(std::move(body), context.routing, context.patches.finish());
}

} // namespace fiber::ai_server
