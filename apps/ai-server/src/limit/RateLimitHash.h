#ifndef FIBER_AI_SERVER_RATE_LIMIT_HASH_H
#define FIBER_AI_SERVER_RATE_LIMIT_HASH_H

#include <cstdint>
#include <string_view>

namespace fiber::ai_server {

[[nodiscard]] std::uint64_t rate_limit_hash64(std::string_view value) noexcept;
[[nodiscard]] std::uint64_t rate_limit_key_hash64(std::string_view user_id, std::string_view model_name) noexcept;

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_RATE_LIMIT_HASH_H
