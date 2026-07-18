#include "Appender.h"

#include <array>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <string_view>
#include <unistd.h>
#include <utility>

#include "../event/EventLoop.h"
#include "LogContext.h"

namespace fiber::log {
namespace {

constexpr std::size_t kFormattedLineCapacity = 9216;

class FixedWriter {
public:
    FixedWriter(char *data, std::size_t capacity) noexcept : data_(data), capacity_(capacity) {}

    void append(std::string_view value) noexcept {
        const std::size_t copy = value.size() < remaining() ? value.size() : remaining();
        if (copy > 0) {
            std::memcpy(data_ + size_, value.data(), copy);
            size_ += copy;
        }
    }

    void append(char value) noexcept {
        if (remaining() > 0) {
            data_[size_++] = value;
        }
    }

    template<typename T>
    void append_integer(T value) noexcept {
        char buffer[32];
        auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
        if (result.ec == std::errc()) {
            append(std::string_view(buffer, static_cast<std::size_t>(result.ptr - buffer)));
        }
    }

    void append_six_digits(std::uint32_t value) noexcept {
        char buffer[6];
        for (std::size_t i = sizeof(buffer); i > 0; --i) {
            buffer[i - 1] = static_cast<char>('0' + value % 10);
            value /= 10;
        }
        append(std::string_view(buffer, sizeof(buffer)));
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
    [[nodiscard]] std::size_t remaining() const noexcept { return capacity_ - size_; }

    char *data_;
    std::size_t capacity_;
    std::size_t size_ = 0;
};

std::string_view source_basename(std::string_view file) noexcept {
    const std::size_t slash = file.find_last_of("/\\");
    return slash == std::string_view::npos ? file : file.substr(slash + 1);
}

std::size_t format_event(const LogEvent &event, char *data, std::size_t capacity) noexcept {
    if (capacity == 0) {
        return 0;
    }
    FixedWriter writer(data, capacity - 1);

    const std::time_t seconds = static_cast<std::time_t>(event.timestamp_us / 1000000);
    const auto micros = static_cast<std::uint32_t>(event.timestamp_us % 1000000);
    std::tm local{};
    char date[32];
    char zone[8];
    if (::localtime_r(&seconds, &local) != nullptr) {
        const std::size_t date_size = std::strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &local);
        writer.append(std::string_view(date, date_size));
        writer.append('.');
        writer.append_six_digits(micros);
        const std::size_t zone_size = std::strftime(zone, sizeof(zone), "%z", &local);
        if (zone_size == 5) {
            writer.append(std::string_view(zone, 3));
            writer.append(':');
            writer.append(std::string_view(zone + 3, 2));
        } else {
            writer.append(std::string_view(zone, zone_size));
        }
    } else {
        writer.append_integer(event.timestamp_us);
    }

    writer.append(' ');
    writer.append(log_level_name(event.level));
    writer.append(" [worker=");
    writer.append_integer(event.thread_id);
    writer.append("] ");
    writer.append(event.logger_name);
    writer.append(' ');
    writer.append(source_basename(event.file));
    writer.append(':');
    writer.append_integer(event.line);
    writer.append(' ');
    writer.append(event.message);
    data[writer.size()] = '\n';
    return writer.size() + 1;
}

std::chrono::steady_clock::time_point current_steady_time() noexcept {
    if (auto *loop = event::EventLoop::current_or_null()) {
        return loop->now();
    }
    return std::chrono::steady_clock::now();
}

void report_raw_error(std::atomic<std::uint64_t> &last_report_second, std::string_view message) noexcept {
    const auto now =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch())
                    .count();
    const auto current_second = static_cast<std::uint64_t>(now);
    std::uint64_t previous = last_report_second.load(std::memory_order_relaxed);
    for (;;) {
        if (previous == current_second) {
            return;
        }
        if (last_report_second.compare_exchange_weak(previous, current_second, std::memory_order_relaxed,
                                                     std::memory_order_relaxed)) {
            break;
        }
    }
    (void) ::write(STDERR_FILENO, message.data(), message.size());
}

} // namespace

Appender::Appender(AppenderId id, std::string name, LogLevel min_level, LogLevel max_level) noexcept :
    id_(id), name_(std::move(name)), min_level_(min_level), max_level_(max_level) {}

AppenderStats Appender::stats() const noexcept {
    return AppenderStats{
            .written_records = written_records_.load(std::memory_order_relaxed),
            .written_bytes = written_bytes_.load(std::memory_order_relaxed),
            .dropped_records = dropped_records_.load(std::memory_order_relaxed),
            .write_errors = write_errors_.load(std::memory_order_relaxed),
            .reopen_errors = reopen_errors_.load(std::memory_order_relaxed),
    };
}

void Appender::record_write(std::size_t bytes, std::uint64_t records) noexcept {
    written_records_.fetch_add(records, std::memory_order_relaxed);
    written_bytes_.fetch_add(bytes, std::memory_order_relaxed);
}

void Appender::record_write_error(std::uint64_t dropped_records) noexcept {
    write_errors_.fetch_add(1, std::memory_order_relaxed);
    dropped_records_.fetch_add(dropped_records, std::memory_order_relaxed);
    report_raw_error(last_diagnostic_second_, "fiber logging write failed\n");
}

void Appender::record_reopen_error() noexcept {
    reopen_errors_.fetch_add(1, std::memory_order_relaxed);
    report_raw_error(last_diagnostic_second_, "fiber logging reopen failed\n");
}

ConsoleAppender::ConsoleAppender(AppenderId id, ConsoleAppenderOptions options) noexcept :
    Appender(id, std::move(options.name), options.min_level, options.max_level),
    fd_(options.stream == ConsoleStream::Stdout ? STDOUT_FILENO : STDERR_FILENO) {}

void ConsoleAppender::append(const LogEvent &event, LogContext &) noexcept {
    std::array<char, kFormattedLineCapacity> line{};
    const std::size_t size = format_event(event, line.data(), line.size());
    const ssize_t written = ::write(fd_, line.data(), size);
    if (written != static_cast<ssize_t>(size)) {
        record_write_error(1);
        return;
    }
    record_write(size, 1);
}

void ConsoleAppender::flush(LogContext &) noexcept {}

void ConsoleAppender::flush_due(LogContext &, std::chrono::steady_clock::time_point) noexcept {}

bool ConsoleAppender::reopen() noexcept { return true; }

FileAppender::FileAppender(AppenderId id, FileAppenderOptions options, std::uint16_t buffer_slot) noexcept :
    Appender(id, std::move(options.name), options.min_level, options.max_level), path_(std::move(options.path)),
    file_mode_(options.file_mode), buffer_size_(options.buffer_size), flush_interval_(options.flush_interval),
    buffer_slot_(buffer_slot) {}

FileAppender::~FileAppender() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

bool FileAppender::open_file(int &system_error) noexcept {
    fd_ = ::open(path_.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, file_mode_);
    if (fd_ < 0) {
        system_error = errno;
        return false;
    }
    system_error = 0;
    return true;
}

void FileAppender::write_bytes(const char *data, std::size_t size, std::uint64_t records) noexcept {
    const ssize_t written = ::write(fd_, data, size);
    if (written != static_cast<ssize_t>(size)) {
        record_write_error(records);
        return;
    }
    record_write(size, records);
}

void FileAppender::flush_buffer(LogBuffer &buffer) noexcept {
    if (buffer.size == 0) {
        return;
    }
    write_bytes(buffer.data, buffer.size, buffer.records);
    buffer.size = 0;
    buffer.records = 0;
    buffer.flush_at = {};
}

void FileAppender::append(const LogEvent &event, LogContext &context) noexcept {
    std::array<char, kFormattedLineCapacity> line{};
    const std::size_t size = format_event(event, line.data(), line.size());

    if (!buffered()) {
        write_bytes(line.data(), size, 1);
        return;
    }

    LogBuffer *buffer = context.buffer_at(buffer_slot_);
    if (!buffer) {
        write_bytes(line.data(), size, 1);
        return;
    }
    if (!buffer->data) {
        if (buffer->owner == this && buffer->capacity == 0) {
            write_bytes(line.data(), size, 1);
            return;
        }
        buffer->data = static_cast<char *>(std::malloc(buffer_size_));
        if (!buffer->data) {
            buffer->owner = this;
            buffer->capacity = 0;
            write_bytes(line.data(), size, 1);
            return;
        }
        buffer->owner = this;
        buffer->capacity = buffer_size_;
    }
    if (buffer->owner != this) {
        write_bytes(line.data(), size, 1);
        return;
    }

    const auto now = current_steady_time();
    if (buffer->size > 0 && now >= buffer->flush_at) {
        flush_buffer(*buffer);
    }
    if (size > buffer->capacity) {
        write_bytes(line.data(), size, 1);
        return;
    }
    if (size > buffer->capacity - buffer->size) {
        flush_buffer(*buffer);
    }
    if (buffer->size == 0) {
        buffer->flush_at = now + flush_interval_;
    }
    std::memcpy(buffer->data + buffer->size, line.data(), size);
    buffer->size += size;
    ++buffer->records;
    if (buffer->size == buffer->capacity) {
        flush_buffer(*buffer);
    }
}

void FileAppender::flush(LogContext &context) noexcept {
    if (!buffered()) {
        return;
    }
    LogBuffer *buffer = context.buffer_at(buffer_slot_);
    if (buffer && buffer->owner == this) {
        flush_buffer(*buffer);
    }
}

void FileAppender::flush_due(LogContext &context, std::chrono::steady_clock::time_point now) noexcept {
    if (!buffered()) {
        return;
    }
    LogBuffer *buffer = context.buffer_at(buffer_slot_);
    if (buffer && buffer->owner == this && buffer->size > 0 && now >= buffer->flush_at) {
        flush_buffer(*buffer);
    }
}

bool FileAppender::reopen() noexcept {
    const int replacement = ::open(path_.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, file_mode_);
    if (replacement < 0) {
        record_reopen_error();
        return false;
    }
    if (::dup3(replacement, fd_, O_CLOEXEC) < 0) {
        ::close(replacement);
        record_reopen_error();
        return false;
    }
    ::close(replacement);
    return true;
}

} // namespace fiber::log
