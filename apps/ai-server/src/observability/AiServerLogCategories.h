#ifndef FIBER_AI_SERVER_AI_SERVER_LOG_CATEGORIES_H
#define FIBER_AI_SERVER_AI_SERVER_LOG_CATEGORIES_H

#include <array>
#include <string_view>

namespace fiber::ai_server {

inline constexpr std::string_view kAiServerLoggerRoot = "ai_server";
inline constexpr std::string_view kAiServerLifecycleLogger = "ai_server.lifecycle";
inline constexpr std::string_view kAiServerConfigLogger = "ai_server.config";
inline constexpr std::string_view kAiServerHttpLogger = "ai_server.http";
inline constexpr std::string_view kAiServerLlmLogger = "ai_server.llm";
inline constexpr std::string_view kAiServerDiscoveryLogger = "ai_server.discovery";
inline constexpr std::string_view kAiServerRateLimitLogger = "ai_server.rate_limit";
inline constexpr std::string_view kAiServerAuditLogger = "ai_server.audit";

inline constexpr std::array kAiServerConfigurableLoggers{
        kAiServerLoggerRoot, kAiServerLifecycleLogger, kAiServerConfigLogger,    kAiServerHttpLogger,
        kAiServerLlmLogger,  kAiServerDiscoveryLogger, kAiServerRateLimitLogger,
};

[[nodiscard]] constexpr bool configurable_ai_server_logger(std::string_view name) noexcept {
    for (std::string_view candidate: kAiServerConfigurableLoggers) {
        if (candidate == name) {
            return true;
        }
    }
    return false;
}

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_AI_SERVER_LOG_CATEGORIES_H
