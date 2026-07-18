#ifndef FIBER_LOG_LOG_CONTEXT_H
#define FIBER_LOG_LOG_CONTEXT_H

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "../event/EventLoop.h"

namespace fiber::log {

class Appender;
class Logger;
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
    friend class Logger;
    friend class LoggerManager;

    class FormatScratchLease {
    public:
        ~FormatScratchLease() noexcept;

        FormatScratchLease(const FormatScratchLease &) = delete;
        FormatScratchLease &operator=(const FormatScratchLease &) = delete;
        FormatScratchLease(FormatScratchLease &&) = delete;
        FormatScratchLease &operator=(FormatScratchLease &&) = delete;

        [[nodiscard]] explicit operator bool() const noexcept { return data_ != nullptr; }
        [[nodiscard]] char *data() const noexcept { return data_; }

    private:
        friend class LogContext;

        FormatScratchLease() noexcept = default;
        FormatScratchLease(LogContext &context, char *data) noexcept : context_(&context), data_(data) {}

        LogContext *context_ = nullptr;
        char *data_ = nullptr;
    };

    [[nodiscard]] bool prepare(std::uint64_t generation, std::uint16_t buffer_count) noexcept;
    void reset() noexcept;
    [[nodiscard]] LogBuffer *buffer_at(std::uint16_t index) noexcept;
    [[nodiscard]] FormatScratchLease acquire_format_scratch() noexcept;
    void release_format_scratch() noexcept;
    void attach_loop(event::EventLoop &loop, std::chrono::milliseconds interval) noexcept;
    void arm_timer(std::chrono::milliseconds interval) noexcept;
    void detach_loop() noexcept;
    static void on_flush_timer(LogContext *context) noexcept;

    LogBuffer *buffers_ = nullptr;
    std::uint16_t buffer_count_ = 0;
    std::uint64_t generation_ = 0;
    event::EventLoop *loop_ = nullptr;
    event::EventLoop::TimerEntry flush_timer_{};
    char *format_scratch_ = nullptr;
    bool timer_armed_ = false;
    bool format_scratch_in_use_ = false;
    bool format_scratch_allocation_failed_ = false;
};

} // namespace fiber::log

#endif // FIBER_LOG_LOG_CONTEXT_H
