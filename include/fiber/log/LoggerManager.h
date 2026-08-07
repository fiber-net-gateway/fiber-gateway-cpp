#ifndef FIBER_LOG_LOGGER_MANAGER_H
#define FIBER_LOG_LOGGER_MANAGER_H

#include <memory>
#include <string_view>

#include "Appender.h"
#include "LogConfig.h"
#include "LogWorker.h"

namespace fiber::log {

class Logger;
class OwnedLogRecord;

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
    [[nodiscard]] bool submit(OwnedLogRecord *record) noexcept;
    void record_allocation_failure() noexcept;
    [[nodiscard]] bool reopen_all() noexcept;
    void flush() noexcept;
    void flush_current_thread() noexcept { flush(); }

    [[nodiscard]] AppenderStats appender_stats(AppenderId id) const noexcept;
    [[nodiscard]] LogQueueStats queue_stats() const noexcept;

private:
    struct Runtime;

    LoggerManager() noexcept;

    std::unique_ptr<Runtime> runtime_;
};

} // namespace fiber::log

#endif // FIBER_LOG_LOGGER_MANAGER_H
