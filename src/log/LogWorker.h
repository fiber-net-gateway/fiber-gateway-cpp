#ifndef FIBER_LOG_LOG_WORKER_H
#define FIBER_LOG_LOG_WORKER_H

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include "../event/EventLoopGroup.h"
#include "Appender.h"
#include "LogConfig.h"
#include "LogFormatter.h"
#include "LogRecord.h"

namespace fiber::log {

struct LogQueueStats {
    std::uint64_t queued_records = 0;
    std::uint64_t queued_bytes = 0;
    std::uint64_t peak_queued_records = 0;
    std::uint64_t peak_queued_bytes = 0;
    std::uint64_t dropped_records = 0;
    std::uint64_t allocation_failures = 0;
    std::uint64_t formatting_failures = 0;
    std::uint64_t writer_thread_id = 0;
    bool accepting = false;
};

class LogBacklog {
public:
    explicit LogBacklog(AsyncLogOptions options) noexcept;

    [[nodiscard]] bool admit(OwnedLogRecord &record) noexcept;
    void release(std::size_t bytes, bool oversized) noexcept;
    void stop_accepting() noexcept;
    void record_allocation_failure() noexcept;
    void record_formatting_failure() noexcept;
    void set_writer_thread_id(std::uint64_t id) noexcept;
    [[nodiscard]] LogQueueStats stats() const noexcept;

private:
    static constexpr std::size_t kOversizedState = std::numeric_limits<std::size_t>::max();

    void record_admission(std::size_t bytes) noexcept;
    void update_peak(std::atomic<std::uint64_t> &peak, std::uint64_t value) noexcept;

    std::atomic<std::size_t> state_{0};
    std::atomic<std::uint64_t> wake_epoch_{0};
    std::atomic<bool> accepting_{true};
    std::size_t capacity_ = 0;
    LogQueueFullPolicy full_policy_ = LogQueueFullPolicy::Block;
    std::atomic<std::uint64_t> queued_records_{0};
    std::atomic<std::uint64_t> queued_bytes_{0};
    std::atomic<std::uint64_t> peak_queued_records_{0};
    std::atomic<std::uint64_t> peak_queued_bytes_{0};
    std::atomic<std::uint64_t> dropped_records_{0};
    std::atomic<std::uint64_t> allocation_failures_{0};
    std::atomic<std::uint64_t> formatting_failures_{0};
    std::atomic<std::uint64_t> writer_thread_id_{0};
};

class LogWorker {
public:
    LogWorker(std::vector<std::unique_ptr<Appender>> &appenders, AsyncLogOptions options) noexcept;
    ~LogWorker();

    LogWorker(const LogWorker &) = delete;
    LogWorker &operator=(const LogWorker &) = delete;
    LogWorker(LogWorker &&) = delete;
    LogWorker &operator=(LogWorker &&) = delete;

    [[nodiscard]] bool start() noexcept;
    void submit(OwnedLogRecord &record) noexcept;
    [[nodiscard]] bool flush() noexcept;
    [[nodiscard]] bool reopen_all() noexcept;
    void stop_accepting() noexcept;
    void stop_after_drain() noexcept;
    void record_allocation_failure() noexcept;
    [[nodiscard]] LogQueueStats queue_stats() const noexcept;
    [[nodiscard]] LogBacklog &backlog() noexcept { return backlog_; }

private:
    enum class ControlKind : std::uint8_t {
        Ready,
        Flush,
        Reopen,
        StopAfterDrain,
    };

    struct Control {
        event::EventLoop::NotifyEntry notify_entry;
        LogWorker *worker = nullptr;
        ControlKind kind = ControlKind::Ready;
        std::atomic<bool> done{false};
        bool result = false;
    };

    [[nodiscard]] bool run_control(ControlKind kind) noexcept;
    void process_record(OwnedLogRecord &record) noexcept;
    void process_control(Control &control) noexcept;
    void flush_due(std::chrono::steady_clock::time_point now) noexcept;
    void flush_all() noexcept;
    void refresh_flush_timer() noexcept;
    void finish_stop() noexcept;

    static void on_record(OwnedLogRecord *record) noexcept;
    static void on_control(Control *control) noexcept;
    static void on_flush_timer(LogWorker *worker) noexcept;
    static void on_stop_deferred(LogWorker *worker) noexcept;

    std::vector<std::unique_ptr<Appender>> *appenders_ = nullptr;
    LogBacklog backlog_;
    event::EventLoopGroup group_{1};
    LogFormatter formatter_;
    event::EventLoop::TimerEntry flush_timer_{};
    event::EventLoop::DeferEntry stop_defer_{};
    Control *stop_control_ = nullptr;
    std::chrono::steady_clock::time_point scheduled_flush_{};
    bool flush_timer_armed_ = false;
    bool started_ = false;
};

} // namespace fiber::log

#endif // FIBER_LOG_LOG_WORKER_H
