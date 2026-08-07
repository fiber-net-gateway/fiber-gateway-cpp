#ifndef FIBER_LOG_LOG_LEVEL_H
#define FIBER_LOG_LOG_LEVEL_H

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace fiber::log {

enum class LogLevel : std::uint8_t {
    Trace = 0,
    Debug,
    Info,
    Warn,
    Error,
    Fatal,
};

inline constexpr std::size_t kLogLevelCount = 6;

[[nodiscard]] constexpr std::size_t level_index(LogLevel level) noexcept { return static_cast<std::size_t>(level); }

[[nodiscard]] constexpr bool valid_log_level(LogLevel level) noexcept { return level_index(level) < kLogLevelCount; }

[[nodiscard]] constexpr std::string_view log_level_name(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace:
            return "TRACE";
        case LogLevel::Debug:
            return "DEBUG";
        case LogLevel::Info:
            return "INFO";
        case LogLevel::Warn:
            return "WARN";
        case LogLevel::Error:
            return "ERROR";
        case LogLevel::Fatal:
            return "FATAL";
    }
    return "UNKNOWN";
}

} // namespace fiber::log

#endif // FIBER_LOG_LOG_LEVEL_H
