#ifndef FIBER_LOG_LOG_CONTEXT_H
#define FIBER_LOG_LOG_CONTEXT_H

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "../event/EventLoop.h"

namespace fiber::log {

class Appender;
class LoggerManager;

struct LogBuffer {
    Appender *owner = nullptr;
    char *data = nullptr;
    std::size_t size = 0;
    std::size_t capacity = 0;
    std::uint64_t records = 0;
    std::chrono::steady_clock::time_point flush_at{};
};

class LogContext {
public:
    LogContext() = default;
    ~LogContext();

    LogContext(const LogContext &) = delete;
    LogContext &operator=(const LogContext &) = delete;
    LogContext(LogContext &&) = delete;
    LogContext &operator=(LogContext &&) = delete;

private:
    friend class FileAppender;
    friend class LoggerManager;

    [[nodiscard]] bool prepare(std::uint64_t generation, std::uint16_t buffer_count) noexcept;
    void reset() noexcept;
    [[nodiscard]] LogBuffer *buffer_at(std::uint16_t index) noexcept;
    void attach_loop(event::EventLoop &loop, std::chrono::milliseconds interval) noexcept;
    void arm_timer(std::chrono::milliseconds interval) noexcept;
    void detach_loop() noexcept;
    static void on_flush_timer(LogContext *context) noexcept;

    LogBuffer *buffers_ = nullptr;
    std::uint16_t buffer_count_ = 0;
    std::uint64_t generation_ = 0;
    event::EventLoop *loop_ = nullptr;
    event::EventLoop::TimerEntry flush_timer_{};
    bool timer_armed_ = false;
};

} // namespace fiber::log

#endif // FIBER_LOG_LOG_CONTEXT_H
