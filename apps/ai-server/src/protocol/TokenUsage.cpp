#include "TokenUsage.h"

#include <array>
#include <limits>

#include <common/Assert.h>
#include <common/json/JsonPath.h>

namespace fiber::ai_server {
namespace {

enum class UsageAction : std::uint32_t {
    Input,
    Output,
    Total,
    CacheCreation,
    CacheRead,
};

struct RawUsage {
    std::optional<std::int64_t> input;
    std::optional<std::int64_t> output;
    std::optional<std::int64_t> total;
    std::optional<std::int64_t> cache_creation;
    std::optional<std::int64_t> cache_read;

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
    if (match.token.kind != json::TokenKind::Integer || match.token.inum < 0) {
        return true;
    }
    auto &usage = *static_cast<RawUsage *>(opaque);
    const std::int64_t value = match.token.inum;
    switch (static_cast<UsageAction>(match.action)) {
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
    }
    return true;
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
            .input_cached = cached,
            .input_uncached = uncached,
            .output = raw.output,
            .total = total,
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
            .input_cached = read,
            .input_uncached = uncached,
            .output = raw.output,
            .total = total,
    };
}

} // namespace

void LlmTokenUsage::merge(const LlmTokenUsage &next) noexcept {
    if (next.input_cached) {
        input_cached = next.input_cached;
    }
    if (next.input_uncached) {
        input_uncached = next.input_uncached;
    }
    if (next.output) {
        output = next.output;
    }
    if (next.total) {
        total = next.total;
    }
    if (!total && input_cached && input_uncached && output) {
        total = add(add(input_cached, input_uncached), output);
    }
}

std::optional<LlmTokenUsage> extract_token_usage(LlmWireProtocol protocol, std::string_view input, bool streaming_event,
                                                 mem::BufPool &pool) noexcept {
    if (input.empty()) {
        return std::nullopt;
    }
    RawUsage raw;
    const json::JsonPathProgram &program =
            protocol == LlmWireProtocol::OpenAiChatCompletions
                    ? openai_program()
                    : (streaming_event ? anthropic_event_program() : anthropic_program());
    auto visited =
            json::visit_json_paths(program, input, pool, json::JsonPathVisitor{.context = &raw, .on_match = &on_usage});
    if (!visited || raw.empty()) {
        return std::nullopt;
    }
    return protocol == LlmWireProtocol::OpenAiChatCompletions ? to_openai(raw) : to_anthropic(raw, streaming_event);
}

} // namespace fiber::ai_server
