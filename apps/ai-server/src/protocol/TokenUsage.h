#ifndef FIBER_AI_SERVER_TOKEN_USAGE_H
#define FIBER_AI_SERVER_TOKEN_USAGE_H

#include "LlmBody.h"

#include <cstdint>
#include <optional>
#include <string_view>

#include <common/mem/BufPool.h>

namespace fiber::ai_server {

struct LlmTokenUsage {
    std::optional<std::int64_t> in_cache;
    std::optional<std::int64_t> in_nocache;
    std::optional<std::int64_t> out;
    std::optional<std::int64_t> total_tokens;

    [[nodiscard]] bool has_usage_fields() const noexcept { return in_cache || in_nocache || out; }

    void merge(const LlmTokenUsage &next) noexcept;
};

struct LlmStreamEventObservation {
    std::optional<LlmTokenUsage> usage;
    bool output_token_observed = false;
};

[[nodiscard]] std::optional<LlmTokenUsage> extract_token_usage(LlmWireProtocol protocol, std::string_view json,
                                                               bool streaming_event, mem::BufPool &pool) noexcept;

[[nodiscard]] LlmStreamEventObservation analyze_stream_event(LlmWireProtocol protocol, std::string_view json,
                                                             mem::BufPool &pool) noexcept;

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_TOKEN_USAGE_H
