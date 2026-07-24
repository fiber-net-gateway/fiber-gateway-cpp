#ifndef FIBER_AI_SERVER_LLM_ERROR_H
#define FIBER_AI_SERVER_LLM_ERROR_H

#include "LlmBody.h"

#include <cstdint>
#include <expected>
#include <string_view>

#include <common/mem/IoBuf.h>

namespace fiber::ai_server {

struct LlmError {
    int status_code = 500;
    std::string_view code = "internal_error";
    std::string_view type = "server_error";
    std::string_view message = "internal server error";
    std::string_view field;
};

enum class LlmErrorEncodeError : std::uint8_t {
    OutOfMemory,
    InvalidUtf8,
    EncodeFailed,
};

[[nodiscard]] std::expected<mem::IoBuf, LlmErrorEncodeError> encode_llm_error(LlmWireProtocol protocol,
                                                                              const LlmError &error) noexcept;

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_LLM_ERROR_H
