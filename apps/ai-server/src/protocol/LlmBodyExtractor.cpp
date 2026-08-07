#include "LlmBody.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>

#include <fiber/common/Assert.h>
#include <fiber/common/json/JsonPath.h>

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
    Tool,
};

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
                {.expression = "$.tools[*tool].function.name", .action = static_cast<std::uint32_t>(BodyAction::Tool)},
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
                {.expression = "$.tools[*tool].name", .action = static_cast<std::uint32_t>(BodyAction::Tool)},
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

struct ExtractContext {
    explicit ExtractContext(std::string_view input, mem::BufPool &pool) noexcept :
        input(input), pool(&pool), roles(pool), contents(pool), patches(pool) {}

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

    void observe_message_index(const JsonPathMatch &match) noexcept {
        const json::JsonPathCapture *capture = match.variables.find("message");
        if (capture && capture->kind == json::JsonPathCaptureKind::ArrayIndex &&
            capture->index != std::numeric_limits<std::size_t>::max()) {
            routing.messages_count = std::max(routing.messages_count, capture->index + 1);
        }
    }

    void observe_tool_index(const JsonPathMatch &match) noexcept {
        const json::JsonPathCapture *capture = match.variables.find("tool");
        if (capture && capture->kind == json::JsonPathCaptureKind::ArrayIndex &&
            capture->index != std::numeric_limits<std::size_t>::max()) {
            routing.tools_count = std::max(routing.tools_count, capture->index + 1);
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
                return true;
            }
            case BodyAction::System:
                return self.read_text_or_null(match, self.routing.system_text);
            case BodyAction::Tool:
                self.observe_tool_index(match);
                return true;
        }
        return self.set_error(LlmBodyErrorCode::InvalidJson, match, {}, "unknown JSON path action");
    }

    std::string_view input;
    mem::BufPool *pool = nullptr;
    LlmRoutingData routing;
    RequestArrayBuilder<Nullable<std::string_view>> roles;
    RequestArrayBuilder<Nullable<std::string_view>> contents;
    RequestArrayBuilder<LlmBodyPatchSite> patches;
    std::optional<LlmBodyError> error;
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

    context.routing.message_roles = context.roles.finish();
    context.routing.message_content_texts = context.contents.finish();
    context.routing.messages_count = std::max(context.routing.messages_count, context.routing.message_roles.size());
    return ParsedLlmBody(std::move(body), context.routing, context.patches.finish());
}

} // namespace fiber::ai_server
