#ifndef FIBER_LOG_LOGGER_MANAGER_H
#define FIBER_LOG_LOGGER_MANAGER_H

#include <cstdint>
#include <memory>
#include <string_view>

#include "Appender.h"
#include "LogConfig.h"
#include "LogContext.h"

namespace fiber::log {

class Logger;

class LoggerManager {
public:
    static LoggerManager &global() noexcept;
    static LoggerManager *try_global() noexcept;

    LoggerManager(const LoggerManager &) = delete;
    LoggerManager &operator=(const LoggerManager &) = delete;
    LoggerManager(LoggerManager &&) = delete;
    LoggerManager &operator=(LoggerManager &&) = delete;

    ~LoggerManager();

    [[nodiscard]] LogConfigResult<void> initialize(LogConfig config);
    void shutdown() noexcept;

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] const Logger *find_logger(std::string_view name) const noexcept;
    [[nodiscard]] bool reopen_all() noexcept;
    void flush_current_thread() noexcept;

    [[nodiscard]] LogContext &current_context() noexcept;
    [[nodiscard]] AppenderStats appender_stats(AppenderId id) const noexcept;

private:
    friend class LogContext;

    struct Runtime;

    LoggerManager() noexcept;

    void flush_context(LogContext &context) noexcept;
    void destroy_context(LogContext &context) noexcept;
    void on_context_timer(LogContext &context) noexcept;

    std::unique_ptr<Runtime> runtime_;
    std::uint64_t next_generation_ = 1;
};

} // namespace fiber::log

#endif // FIBER_LOG_LOGGER_MANAGER_H
