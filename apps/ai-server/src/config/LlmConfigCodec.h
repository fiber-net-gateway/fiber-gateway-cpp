#ifndef FIBER_AI_SERVER_LLM_CONFIG_CODEC_H
#define FIBER_AI_SERVER_LLM_CONFIG_CODEC_H

#include "LlmConfigSnapshot.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace fiber::ai_server {

enum class LlmConfigErrorCode : std::uint8_t {
    InvalidJson,
    InvalidEnvelope,
    MissingField,
    InvalidField,
    DuplicateValue,
};

struct LlmConfigError {
    LlmConfigErrorCode code = LlmConfigErrorCode::InvalidJson;
    std::size_t offset = 0;
    std::string field;
    std::string message;
};

[[nodiscard]] std::expected<Bt1KeySnapshot, LlmConfigError> parse_bt1_key_config(std::string_view content,
                                                                                 std::string_view md5);

[[nodiscard]] std::expected<UserGroupSnapshot, LlmConfigError>
parse_user_group_config(std::string_view content, std::string_view md5, std::string_view expected_name);

[[nodiscard]] std::expected<ProviderConfigSnapshot, LlmConfigError>
parse_provider_config(std::string_view content, std::string_view md5, std::string_view expected_name);

[[nodiscard]] std::expected<ModelsConfigSnapshot, LlmConfigError> parse_models_config(std::string_view content,
                                                                                      std::string_view md5);

[[nodiscard]] bool valid_provider_name(std::string_view name) noexcept;
[[nodiscard]] bool valid_user_group_name(std::string_view name) noexcept;
[[nodiscard]] bool valid_model_name(std::string_view name) noexcept;

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_LLM_CONFIG_CODEC_H
