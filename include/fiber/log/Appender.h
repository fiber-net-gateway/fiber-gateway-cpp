#ifndef FIBER_LOG_APPENDER_H
#define FIBER_LOG_APPENDER_H

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <sys/types.h>
#include <vector>

#include "FormattedLogRecord.h"
#include "LogConfig.h"

namespace fiber::log {

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
    std::uint64_t writer_thread_id = 0;
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

    virtual void append(const FormattedLogRecord &record, std::chrono::steady_clock::time_point now) noexcept = 0;
    virtual void flush() noexcept = 0;
    [[nodiscard]] virtual bool reopen() noexcept = 0;
    [[nodiscard]] virtual std::chrono::steady_clock::time_point flush_deadline() const noexcept {
        return std::chrono::steady_clock::time_point::max();
    }

    void record_drop() noexcept;

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
    std::atomic<std::uint64_t> writer_thread_id_{0};
    std::atomic<std::uint64_t> last_diagnostic_second_{std::numeric_limits<std::uint64_t>::max()};
};

class ConsoleAppender final : public Appender {
public:
    ConsoleAppender(AppenderId id, ConsoleAppenderOptions options) noexcept;

    void append(const FormattedLogRecord &record, std::chrono::steady_clock::time_point now) noexcept override;
    void flush() noexcept override;
    [[nodiscard]] bool reopen() noexcept override;

private:
    int fd_ = -1;
};

class FileAppender final : public Appender {
public:
    FileAppender(AppenderId id, FileAppenderOptions options) noexcept;
    ~FileAppender() override;

    [[nodiscard]] bool open_file(int &system_error) noexcept;

    void append(const FormattedLogRecord &record, std::chrono::steady_clock::time_point now) noexcept override;
    void flush() noexcept override;
    [[nodiscard]] bool reopen() noexcept override;
    [[nodiscard]] std::chrono::steady_clock::time_point flush_deadline() const noexcept override;

    [[nodiscard]] bool buffered() const noexcept { return buffer_size_ != 0; }

private:
    struct ArchiveEntry {
        std::uint64_t sequence = 0;
        std::string path;
    };

    [[nodiscard]] bool initialize_rotation(int &system_error) noexcept;
    [[nodiscard]] int open_flags() const noexcept;
    [[nodiscard]] bool validate_open_file(int fd, std::uint64_t &active_bytes, int &system_error) const noexcept;
    [[nodiscard]] bool recover_incomplete_tail(int fd, std::uint64_t file_size, std::uint64_t &active_bytes,
                                               int &system_error) const noexcept;
    [[nodiscard]] bool should_rotate(std::size_t incoming_size) const noexcept;
    [[nodiscard]] bool rotate() noexcept;
    [[nodiscard]] std::size_t record_size(const FormattedLogRecord &record) const noexcept;
    [[nodiscard]] FormattedLogRecord::Cursor record_cursor(const FormattedLogRecord &record) const noexcept;
    [[nodiscard]] bool copy_record_to(const FormattedLogRecord &record, char *destination,
                                      std::size_t capacity) const noexcept;
    void cleanup_archives(std::chrono::steady_clock::time_point now) noexcept;
    void prepare_record(std::size_t record_size, std::chrono::steady_clock::time_point now) noexcept;
    void write_contiguous(const char *data, std::size_t size, std::uint64_t records) noexcept;
    void write_record(const FormattedLogRecord &record, std::size_t size) noexcept;
    void flush_buffer() noexcept;

    std::string path_;
    std::string directory_path_;
    std::string base_name_;
    mode_t file_mode_ = 0644;
    std::size_t buffer_size_ = 0;
    std::chrono::milliseconds flush_interval_{0};
    std::optional<FileRotationOptions> rotation_;
    FileAppenderLayout layout_ = FileAppenderLayout::Formatted;
    bool no_follow_ = false;
    bool regular_file_only_ = false;
    bool enforce_file_mode_ = false;
    bool truncate_incomplete_tail_ = false;
    std::vector<ArchiveEntry> archives_;
    std::chrono::steady_clock::time_point rotation_retry_after_{};
    std::chrono::steady_clock::time_point cleanup_retry_after_{};
    std::chrono::steady_clock::time_point flush_at_{};
    std::uint64_t active_bytes_ = 0;
    std::uint64_t next_archive_sequence_ = 1;
    std::uint64_t buffered_records_ = 0;
    char *buffer_ = nullptr;
    std::size_t buffer_used_ = 0;
    int fd_ = -1;
};

} // namespace fiber::log

#endif // FIBER_LOG_APPENDER_H
