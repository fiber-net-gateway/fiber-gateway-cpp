#include "TokenUsage.h"

#include <array>
#include <limits>

#include <fiber/common/Assert.h>
#include <fiber/common/json/JsonPath.h>

namespace fiber::ai_server {
namespace {

enum class UsageAction : std::uint32_t {
    Input,
    Output,
    Total,
    CacheCreation,
    CacheRead,
    OutputToken,
};

struct RawUsage {
    std::optional<std::int64_t> input;
    std::optional<std::int64_t> output;
    std::optional<std::int64_t> total;
    std::optional<std::int64_t> cache_creation;
    std::optional<std::int64_t> cache_read;
    bool output_token_observed = false;

    [[nodiscard]] bool empty() const noexcept { return !input && !output && !total && !cache_creation && !cache_read; }
};

const json::JsonPathProgram &openai_program() {
    static const json::JsonPathProgram program = [] {
        constexpr json::JsonPathRule rules[] = {
                {.expression = "$.usage.prompt_tokens", .action = static_cast<std::uint32_t>(UsageAction::Input)},
                {.expression = "$.usage.completion_tokens", .action = static_cast<std::uint32_t>(UsageAction::Output)},
                {.expression = "$.usage.total_tokens", .action = static_cast<std::uint32_t>(UsageAction::Total)},
                {.expression = "$.usage.prompt_tokens_details.cached_tokens",
                 .action = static_cast<std::uint32_t>(UsageAction::CacheRead)},
        };
        auto compiled = json::JsonPathProgram::compile(rules);
        FIBER_ASSERT(compiled.has_value());
        return std::move(*compiled);
    }();
    return program;
}

const json::JsonPathProgram &openai_event_program() {
    static const json::JsonPathProgram program = [] {
        constexpr json::JsonPathRule rules[] = {
                {.expression = "$.usage.prompt_tokens", .action = static_cast<std::uint32_t>(UsageAction::Input)},
                {.expression = "$.usage.completion_tokens", .action = static_cast<std::uint32_t>(UsageAction::Output)},
                {.expression = "$.usage.total_tokens", .action = static_cast<std::uint32_t>(UsageAction::Total)},
                {.expression = "$.usage.prompt_tokens_details.cached_tokens",
                 .action = static_cast<std::uint32_t>(UsageAction::CacheRead)},
                {.expression = "$.choices[*choice].delta.content",
                 .action = static_cast<std::uint32_t>(UsageAction::OutputToken)},
                {.expression = "$.choices[*choice].delta.refusal",
                 .action = static_cast<std::uint32_t>(UsageAction::OutputToken)},
                {.expression = "$.choices[*choice].delta.tool_calls[*tool].function.name",
                 .action = static_cast<std::uint32_t>(UsageAction::OutputToken)},
                {.expression = "$.choices[*choice].delta.tool_calls[*tool].function.arguments",
                 .action = static_cast<std::uint32_t>(UsageAction::OutputToken)},
        };
        auto compiled = json::JsonPathProgram::compile(rules);
        FIBER_ASSERT(compiled.has_value());
        return std::move(*compiled);
    }();
    return program;
}

const json::JsonPathProgram &anthropic_program() {
    static const json::JsonPathProgram program = [] {
        constexpr json::JsonPathRule rules[] = {
                {.expression = "$.usage.input_tokens", .action = static_cast<std::uint32_t>(UsageAction::Input)},
                {.expression = "$.usage.output_tokens", .action = static_cast<std::uint32_t>(UsageAction::Output)},
                {.expression = "$.usage.cache_creation_input_tokens",
                 .action = static_cast<std::uint32_t>(UsageAction::CacheCreation)},
                {.expression = "$.usage.cache_read_input_tokens",
                 .action = static_cast<std::uint32_t>(UsageAction::CacheRead)},
        };
        auto compiled = json::JsonPathProgram::compile(rules);
        FIBER_ASSERT(compiled.has_value());
        return std::move(*compiled);
    }();
    return program;
}

const json::JsonPathProgram &anthropic_event_program() {
    static const json::JsonPathProgram program = [] {
        constexpr json::JsonPathRule rules[] = {
                {.expression = "$.message.usage.input_tokens",
                 .action = static_cast<std::uint32_t>(UsageAction::Input)},
                {.expression = "$.message.usage.output_tokens",
                 .action = static_cast<std::uint32_t>(UsageAction::Output)},
                {.expression = "$.message.usage.cache_creation_input_tokens",
                 .action = static_cast<std::uint32_t>(UsageAction::CacheCreation)},
                {.expression = "$.message.usage.cache_read_input_tokens",
                 .action = static_cast<std::uint32_t>(UsageAction::CacheRead)},
                {.expression = "$.usage.input_tokens", .action = static_cast<std::uint32_t>(UsageAction::Input)},
                {.expression = "$.usage.output_tokens", .action = static_cast<std::uint32_t>(UsageAction::Output)},
                {.expression = "$.usage.cache_creation_input_tokens",
                 .action = static_cast<std::uint32_t>(UsageAction::CacheCreation)},
                {.expression = "$.usage.cache_read_input_tokens",
                 .action = static_cast<std::uint32_t>(UsageAction::CacheRead)},
                {.expression = "$.content_block.text", .action = static_cast<std::uint32_t>(UsageAction::OutputToken)},
                {.expression = "$.content_block.name", .action = static_cast<std::uint32_t>(UsageAction::OutputToken)},
                {.expression = "$.delta.text", .action = static_cast<std::uint32_t>(UsageAction::OutputToken)},
                {.expression = "$.delta.partial_json", .action = static_cast<std::uint32_t>(UsageAction::OutputToken)},
        };
        auto compiled = json::JsonPathProgram::compile(rules);
        FIBER_ASSERT(compiled.has_value());
        return std::move(*compiled);
    }();
    return program;
}

std::optional<std::int64_t> add(std::optional<std::int64_t> left, std::optional<std::int64_t> right) noexcept {
    if (!left || !right || *left > std::numeric_limits<std::int64_t>::max() - *right) {
        return std::nullopt;
    }
    return *left + *right;
}

bool on_usage(void *opaque, const json::JsonPathMatch &match) noexcept {
    auto &usage = *static_cast<RawUsage *>(opaque);
    const auto action = static_cast<UsageAction>(match.action);
    if (action == UsageAction::OutputToken) {
        if (match.token.kind == json::TokenKind::Text && !match.token.view.empty()) {
            usage.output_token_observed = true;
        }
        return true;
    }
    if (match.token.kind != json::TokenKind::Integer || match.token.inum < 0) {
        return true;
    }
    const std::int64_t value = match.token.inum;
    switch (action) {
        case UsageAction::Input:
            usage.input = value;
            break;
        case UsageAction::Output:
            usage.output = value;
            break;
        case UsageAction::Total:
            usage.total = value;
            break;
        case UsageAction::CacheCreation:
            usage.cache_creation = value;
            break;
        case UsageAction::CacheRead:
            usage.cache_read = value;
            break;
        case UsageAction::OutputToken:
            break;
    }
    return true;
}

bool extract_raw(LlmWireProtocol protocol, std::string_view input, bool streaming_event, mem::BufPool &pool,
                 RawUsage &raw) noexcept {
    if (input.empty()) {
        return false;
    }
    const json::JsonPathProgram &program =
            protocol == LlmWireProtocol::OpenAiChatCompletions
                    ? (streaming_event ? openai_event_program() : openai_program())
                    : (streaming_event ? anthropic_event_program() : anthropic_program());
    return json::visit_json_paths(program, input, pool, json::JsonPathVisitor{.context = &raw, .on_match = &on_usage})
            .has_value();
}

std::optional<LlmTokenUsage> to_openai(const RawUsage &raw) noexcept {
    if (!raw.input && !raw.output && !raw.total) {
        return std::nullopt;
    }
    const std::optional<std::int64_t> cached =
            raw.cache_read ? raw.cache_read
                           : ((raw.input || raw.output) ? std::optional<std::int64_t>(0) : std::nullopt);
    std::optional<std::int64_t> uncached;
    if (raw.input && cached && *raw.input >= *cached) {
        uncached = *raw.input - *cached;
    }
    std::optional<std::int64_t> total = raw.total;
    if (!total) {
        total = add(raw.input, raw.output);
    }
    return LlmTokenUsage{
            .in_cache = cached,
            .in_nocache = uncached,
            .out = raw.output,
            .total_tokens = total,
    };
}

std::optional<LlmTokenUsage> to_anthropic(const RawUsage &raw, bool partial) noexcept {
    if (raw.empty()) {
        return std::nullopt;
    }
    const std::optional<std::int64_t> creation =
            raw.cache_creation ? raw.cache_creation : (partial ? std::nullopt : std::optional<std::int64_t>(0));
    const bool has_input_side = raw.input || raw.cache_creation || raw.cache_read;
    const std::optional<std::int64_t> read =
            raw.cache_read
                    ? raw.cache_read
                    : ((!partial || raw.input || raw.cache_creation) ? std::optional<std::int64_t>(0) : std::nullopt);
    std::optional<std::int64_t> uncached;
    if (!partial || has_input_side) {
        uncached = add(raw.input.value_or(0), creation.value_or(0));
    }
    std::optional<std::int64_t> total;
    if (raw.input && raw.output) {
        total = add(raw.input, raw.output);
        total = add(total, creation.value_or(0));
        total = add(total, read.value_or(0));
    }
    return LlmTokenUsage{
            .in_cache = read,
            .in_nocache = uncached,
            .out = raw.output,
            .total_tokens = total,
    };
}

} // namespace

void LlmTokenUsage::merge(const LlmTokenUsage &next) noexcept {
    if (next.in_cache) {
        in_cache = next.in_cache;
    }
    if (next.in_nocache) {
        in_nocache = next.in_nocache;
    }
    if (next.out) {
        out = next.out;
    }
    if (next.total_tokens) {
        total_tokens = next.total_tokens;
    }
    if (!total_tokens && in_cache && in_nocache && out) {
        total_tokens = add(add(in_cache, in_nocache), out);
    }
}

std::optional<LlmTokenUsage> extract_token_usage(LlmWireProtocol protocol, std::string_view input, bool streaming_event,
                                                 mem::BufPool &pool) noexcept {
    RawUsage raw;
    if (!extract_raw(protocol, input, streaming_event, pool, raw) || raw.empty()) {
        return std::nullopt;
    }
    return protocol == LlmWireProtocol::OpenAiChatCompletions ? to_openai(raw) : to_anthropic(raw, streaming_event);
}

LlmStreamEventObservation analyze_stream_event(LlmWireProtocol protocol, std::string_view input,
                                               mem::BufPool &pool) noexcept {
    RawUsage raw;
    if (!extract_raw(protocol, input, true, pool, raw)) {
        return {};
    }
    return LlmStreamEventObservation{
            .usage = raw.empty() ? std::nullopt
                                 : (protocol == LlmWireProtocol::OpenAiChatCompletions ? to_openai(raw)
                                                                                       : to_anthropic(raw, true)),
            .output_token_observed = raw.output_token_observed,
    };
}

} // namespace fiber::ai_server
