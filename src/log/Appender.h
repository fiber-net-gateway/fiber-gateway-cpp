#ifndef FIBER_LOG_APPENDER_H
#define FIBER_LOG_APPENDER_H

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <sys/types.h>
#include <vector>

#include "FormattedLogLine.h"
#include "LogConfig.h"

namespace fiber::log {

class LogContext;
struct LogBuffer;

struct AppenderStats {
    std::uint64_t written_records = 0;
    std::uint64_t written_bytes = 0;
    std::uint64_t dropped_records = 0;
    std::uint64_t write_errors = 0;
    std::uint64_t reopen_errors = 0;
    std::uint64_t rotations = 0;
    std::uint64_t rotation_errors = 0;
    std::uint64_t retention_errors = 0;
    std::uint64_t active_file_bytes = 0;
};

class Appender {
public:
    virtual ~Appender() = default;

    Appender(const Appender &) = delete;
    Appender &operator=(const Appender &) = delete;
    Appender(Appender &&) = delete;
    Appender &operator=(Appender &&) = delete;

    [[nodiscard]] AppenderId id() const noexcept { return id_; }
    [[nodiscard]] std::string_view name() const noexcept { return name_; }
    [[nodiscard]] bool accepts(LogLevel level) const noexcept {
        return valid_log_level(level) && level_index(level) >= level_index(min_level_) &&
               level_index(level) <= level_index(max_level_);
    }
    [[nodiscard]] AppenderStats stats() const noexcept;

    virtual void append(FormattedLogLine line, LogContext &context) noexcept = 0;
    virtual void flush(LogContext &context) noexcept = 0;
    virtual void flush_due(LogContext &context, std::chrono::steady_clock::time_point now) noexcept = 0;
    [[nodiscard]] virtual bool reopen() noexcept = 0;

protected:
    Appender(AppenderId id, std::string name, LogLevel min_level, LogLevel max_level) noexcept;

    void record_write(std::size_t bytes, std::uint64_t records) noexcept;
    void record_write_error(std::uint64_t dropped_records) noexcept;
    void record_reopen_error() noexcept;
    void record_rotation() noexcept;
    void record_rotation_error() noexcept;
    void record_retention_error() noexcept;
    void set_active_file_bytes(std::uint64_t bytes) noexcept;

private:
    AppenderId id_;
    std::string name_;
    LogLevel min_level_;
    LogLevel max_level_;
    std::atomic<std::uint64_t> written_records_{0};
    std::atomic<std::uint64_t> written_bytes_{0};
    std::atomic<std::uint64_t> dropped_records_{0};
    std::atomic<std::uint64_t> write_errors_{0};
    std::atomic<std::uint64_t> reopen_errors_{0};
    std::atomic<std::uint64_t> rotations_{0};
    std::atomic<std::uint64_t> rotation_errors_{0};
    std::atomic<std::uint64_t> retention_errors_{0};
    std::atomic<std::uint64_t> active_file_bytes_{0};
    std::atomic<std::uint64_t> last_diagnostic_second_{std::numeric_limits<std::uint64_t>::max()};
};

class ConsoleAppender final : public Appender {
public:
    ConsoleAppender(AppenderId id, ConsoleAppenderOptions options) noexcept;

    void append(FormattedLogLine line, LogContext &context) noexcept override;
    void flush(LogContext &context) noexcept override;
    void flush_due(LogContext &context, std::chrono::steady_clock::time_point now) noexcept override;
    [[nodiscard]] bool reopen() noexcept override;

private:
    int fd_ = -1;
};

class FileAppender final : public Appender {
public:
    static constexpr std::uint16_t kNoBufferSlot = static_cast<std::uint16_t>(-1);

    FileAppender(AppenderId id, FileAppenderOptions options, std::uint16_t buffer_slot) noexcept;
    ~FileAppender() override;

    [[nodiscard]] bool open_file(int &system_error) noexcept;

    void append(FormattedLogLine line, LogContext &context) noexcept override;
    void flush(LogContext &context) noexcept override;
    void flush_due(LogContext &context, std::chrono::steady_clock::time_point now) noexcept override;
    [[nodiscard]] bool reopen() noexcept override;

    [[nodiscard]] bool buffered() const noexcept { return buffer_slot_ != kNoBufferSlot; }
    [[nodiscard]] std::uint16_t buffer_slot() const noexcept { return buffer_slot_; }

private:
    struct ArchiveEntry {
        std::uint64_t sequence = 0;
        std::string path;
    };

    [[nodiscard]] bool initialize_rotation(int &system_error) noexcept;
    [[nodiscard]] bool should_rotate_locked(std::size_t incoming_size) const noexcept;
    [[nodiscard]] bool rotate_locked() noexcept;
    void select_cleanup_locked(std::uint64_t &sequence, std::string &path) noexcept;
    void finish_cleanup(std::uint64_t sequence, std::string path) noexcept;
    void write_bytes(const char *data, std::size_t size, std::uint64_t records) noexcept;
    void write_bytes_locked(const char *data, std::size_t size, std::uint64_t records) noexcept;
    void flush_buffer(LogBuffer &buffer) noexcept;

    std::string path_;
    std::string directory_path_;
    std::string base_name_;
    mode_t file_mode_ = 0644;
    std::size_t buffer_size_ = 0;
    std::chrono::milliseconds flush_interval_{0};
    std::uint16_t buffer_slot_ = kNoBufferSlot;
    std::optional<FileRotationOptions> rotation_;
    std::vector<ArchiveEntry> archives_;
    std::mutex io_mutex_;
    std::chrono::steady_clock::time_point rotation_retry_after_{};
    std::chrono::steady_clock::time_point cleanup_retry_after_{};
    std::uint64_t active_bytes_ = 0;
    std::uint64_t next_archive_sequence_ = 1;
    std::uint64_t cleanup_in_progress_sequence_ = 0;
    int fd_ = -1;
};

} // namespace fiber::log

#endif // FIBER_LOG_APPENDER_H
