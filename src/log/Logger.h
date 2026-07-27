#ifndef FIBER_LOG_LOGGER_H
#define FIBER_LOG_LOGGER_H

#include <array>
#include <cstdint>
#include <string_view>

#include "LogEvent.h"

namespace fiber::log {

class Appender;
class LogContext;
class LoggerManager;

struct LevelTargets {
    Appender *const *first = nullptr;
    std::uint32_t count = 0;

    [[nodiscard]] bool empty() const noexcept { return count == 0; }
};

class Logger {
public:
    [[nodiscard]] std::string_view name() const noexcept { return name_; }
    [[nodiscard]] const LevelTargets &targets(LogLevel level) const noexcept {
        return valid_log_level(level) ? levels_[level_index(level)] : empty_targets_;
    }
    [[nodiscard]] bool enabled(LogLevel level) const noexcept {
        return valid_log_level(level) && !levels_[level_index(level)].empty();
    }
    [[nodiscard]] bool vlog_enabled(unsigned verbosity) const noexcept {
        return verbosity <= verbosity_ && enabled(LogLevel::Debug);
    }

    void dispatch(const LogEvent &event, LogContext &context) const noexcept;
    [[nodiscard]] bool dispatch_complete(const LogEvent &event, LogContext &context) const noexcept;

private:
    friend class LoggerManager;
    friend const Logger &bootstrap_logger() noexcept;

    explicit Logger(std::string_view name) noexcept : name_(name) {}

    std::string_view name_;
    std::array<LevelTargets, kLogLevelCount> levels_{};
    unsigned verbosity_ = 0;
    inline static constexpr LevelTargets empty_targets_{};
};

class LoggerHandle {
public:
    explicit LoggerHandle(std::string_view name) noexcept;

    [[nodiscard]] const Logger &get() const noexcept { return *logger_; }
    [[nodiscard]] std::string_view name() const noexcept { return name_; }

private:
    friend class LoggerManager;

    std::string_view name_;
    const Logger *logger_ = nullptr;
    LoggerHandle *registry_next_ = nullptr;
};

[[nodiscard]] const Logger &bootstrap_logger() noexcept;

namespace detail {

[[nodiscard]] LoggerHandle *logger_registry_head() noexcept;
void seal_logger_registry() noexcept;
[[nodiscard]] bool late_logger_registration() noexcept;

} // namespace detail

} // namespace fiber::log

#endif // FIBER_LOG_LOGGER_H
