#ifndef FIBER_AI_SERVER_TOKEN_USAGE_H
#define FIBER_AI_SERVER_TOKEN_USAGE_H

#include "LlmBody.h"

#include <cstdint>
#include <optional>
#include <string_view>

#include <common/mem/BufPool.h>

namespace fiber::ai_server {

struct LlmTokenUsage {
    std::optional<std::int64_t> input_cached;
    std::optional<std::int64_t> input_uncached;
    std::optional<std::int64_t> output;
    std::optional<std::int64_t> total;

    [[nodiscard]] bool has_usage_fields() const noexcept { return input_cached || input_uncached || output; }

    void merge(const LlmTokenUsage &next) noexcept;
};

[[nodiscard]] std::optional<LlmTokenUsage> extract_token_usage(LlmWireProtocol protocol, std::string_view json,
                                                               bool streaming_event, mem::BufPool &pool) noexcept;

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_TOKEN_USAGE_H
