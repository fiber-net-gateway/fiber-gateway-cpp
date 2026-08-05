#ifndef FIBER_AI_SERVER_LLM_CONFIG_STATUS_H
#define FIBER_AI_SERVER_LLM_CONFIG_STATUS_H

#include "LlmConfigSnapshot.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace fiber::ai_server {

[[nodiscard]] std::string render_llm_config_status(const LlmConfigSnapshot &snapshot, std::size_t worker_index,
                                                   std::span<const std::uint64_t> worker_generations);

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_LLM_CONFIG_STATUS_H
