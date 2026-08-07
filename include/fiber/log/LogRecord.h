#ifndef FIBER_LOG_LOG_RECORD_H
#define FIBER_LOG_LOG_RECORD_H

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "../event/EventLoop.h"
#include "LogConfig.h"
#include "LogLevel.h"

namespace fiber::log {

class LogWorker;
class LogBacklog;

struct LogMessageChunk {
    LogMessageChunk *next = nullptr;
    std::size_t size = 0;
    std::size_t capacity = 0;
    char data[1];
};

class OwnedLogRecord {
public:
    static constexpr std::size_t kInlineMessageCapacity = 768;

    [[nodiscard]] static OwnedLogRecord *create(std::string_view logger_name, const AppenderId *targets,
                                                std::uint32_t target_count, LogLevel level, const char *file,
                                                std::uint32_t line, const char *function, std::uint64_t timestamp_us,
                                                std::uint32_t thread_id) noexcept;

    ~OwnedLogRecord();

    OwnedLogRecord(const OwnedLogRecord &) = delete;
    OwnedLogRecord &operator=(const OwnedLogRecord &) = delete;
    OwnedLogRecord(OwnedLogRecord &&) = delete;
    OwnedLogRecord &operator=(OwnedLogRecord &&) = delete;

    [[nodiscard]] bool append(std::string_view value) noexcept;

    [[nodiscard]] std::string_view logger_name() const noexcept { return logger_name_; }
    [[nodiscard]] const AppenderId *targets() const noexcept { return targets_; }
    [[nodiscard]] std::uint32_t target_count() const noexcept { return target_count_; }
    [[nodiscard]] LogLevel level() const noexcept { return level_; }
    [[nodiscard]] std::string_view file() const noexcept {
        return file_ ? std::string_view(file_) : std::string_view();
    }
    [[nodiscard]] std::string_view function() const noexcept {
        return function_ ? std::string_view(function_) : std::string_view();
    }
    [[nodiscard]] std::uint32_t line() const noexcept { return line_; }
    [[nodiscard]] std::uint64_t timestamp_us() const noexcept { return timestamp_us_; }
    [[nodiscard]] std::uint32_t thread_id() const noexcept { return thread_id_; }
    [[nodiscard]] const char *inline_message() const noexcept { return inline_message_; }
    [[nodiscard]] std::size_t inline_message_size() const noexcept { return inline_message_size_; }
    [[nodiscard]] const LogMessageChunk *first_chunk() const noexcept { return first_chunk_; }
    [[nodiscard]] std::size_t message_size() const noexcept { return message_size_; }
    [[nodiscard]] std::size_t allocated_bytes() const noexcept { return allocated_bytes_; }
    [[nodiscard]] bool failed() const noexcept { return failed_; }

private:
    friend class LogWorker;
    friend class LogBacklog;

    OwnedLogRecord(std::string_view logger_name, const AppenderId *targets, std::uint32_t target_count, LogLevel level,
                   const char *file, std::uint32_t line, const char *function, std::uint64_t timestamp_us,
                   std::uint32_t thread_id) noexcept;

    [[nodiscard]] bool allocate_chunk() noexcept;

    event::EventLoop::NotifyEntry notify_entry_{};
    std::string_view logger_name_;
    const AppenderId *targets_ = nullptr;
    std::uint32_t target_count_ = 0;
    LogLevel level_ = LogLevel::Info;
    const char *file_ = nullptr;
    const char *function_ = nullptr;
    std::uint32_t line_ = 0;
    std::uint64_t timestamp_us_ = 0;
    std::uint32_t thread_id_ = 0;
    std::size_t message_size_ = 0;
    std::size_t allocated_bytes_ = sizeof(OwnedLogRecord);
    std::size_t inline_message_size_ = 0;
    std::size_t next_chunk_capacity_ = 4096;
    LogMessageChunk *first_chunk_ = nullptr;
    LogMessageChunk *last_chunk_ = nullptr;
    LogWorker *worker_ = nullptr;
    bool failed_ = false;
    bool oversized_admission_ = false;
    char inline_message_[kInlineMessageCapacity];
};

} // namespace fiber::log

#endif // FIBER_LOG_LOG_RECORD_H
