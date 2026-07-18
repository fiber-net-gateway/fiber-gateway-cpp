#include "Appender.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <linux/fs.h>
#include <string_view>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>

#include "../event/EventLoop.h"
#include "LogContext.h"

namespace fiber::log {
namespace {

constexpr std::chrono::seconds kRotationRetryDelay{1};
constexpr std::size_t kUtcArchiveStampSize = 16;
constexpr std::size_t kMinArchiveSequenceDigits = 6;
constexpr unsigned kMaxArchiveNameAttempts = 1024;

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

bool valid_utc_archive_stamp(std::string_view value) noexcept {
    if (value.size() != kUtcArchiveStampSize || value[8] != 'T' || value[15] != 'Z') {
        return false;
    }
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (i == 8 || i == 15) {
            continue;
        }
        if (value[i] < '0' || value[i] > '9') {
            return false;
        }
    }
    return true;
}

std::size_t fixed_pattern_size(std::string_view pattern, std::string_view base_name) noexcept {
    std::size_t size = 0;
    for (std::size_t i = 0; i < pattern.size();) {
        if (pattern.substr(i).starts_with("{base}")) {
            size += base_name.size();
            i += 6;
        } else if (pattern.substr(i).starts_with("{utc}")) {
            size += kUtcArchiveStampSize;
            i += 5;
        } else {
            ++size;
            ++i;
        }
    }
    return size;
}

bool match_fixed_pattern(std::string_view pattern, std::string_view value, std::string_view base_name) noexcept {
    std::size_t value_pos = 0;
    for (std::size_t i = 0; i < pattern.size();) {
        if (pattern.substr(i).starts_with("{base}")) {
            if (!value.substr(value_pos).starts_with(base_name)) {
                return false;
            }
            value_pos += base_name.size();
            i += 6;
            continue;
        }
        if (pattern.substr(i).starts_with("{utc}")) {
            if (!valid_utc_archive_stamp(value.substr(value_pos, kUtcArchiveStampSize))) {
                return false;
            }
            value_pos += kUtcArchiveStampSize;
            i += 5;
            continue;
        }
        if (value_pos >= value.size() || value[value_pos] != pattern[i]) {
            return false;
        }
        ++value_pos;
        ++i;
    }
    return value_pos == value.size();
}

bool match_archive_name(std::string_view pattern, std::string_view name, std::string_view base_name,
                        std::uint64_t &sequence) noexcept {
    const std::size_t sequence_token = pattern.find("{seq}");
    if (sequence_token == std::string_view::npos) {
        return false;
    }
    const std::string_view prefix_pattern = pattern.substr(0, sequence_token);
    const std::string_view suffix_pattern = pattern.substr(sequence_token + 5);
    const std::size_t prefix_size = fixed_pattern_size(prefix_pattern, base_name);
    const std::size_t suffix_size = fixed_pattern_size(suffix_pattern, base_name);
    if (name.size() < prefix_size + suffix_size + kMinArchiveSequenceDigits) {
        return false;
    }
    if (!match_fixed_pattern(prefix_pattern, name.substr(0, prefix_size), base_name) ||
        !match_fixed_pattern(suffix_pattern, name.substr(name.size() - suffix_size), base_name)) {
        return false;
    }

    const std::string_view sequence_text = name.substr(prefix_size, name.size() - prefix_size - suffix_size);
    auto result = std::from_chars(sequence_text.data(), sequence_text.data() + sequence_text.size(), sequence);
    return result.ec == std::errc() && result.ptr == sequence_text.data() + sequence_text.size() && sequence != 0;
}

bool format_utc_archive_stamp(char (&output)[kUtcArchiveStampSize + 1]) noexcept {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
    return ::gmtime_r(&now, &utc) != nullptr &&
           std::strftime(output, sizeof(output), "%Y%m%dT%H%M%SZ", &utc) == kUtcArchiveStampSize;
}

void append_archive_sequence(std::string &output, std::uint64_t sequence) {
    char buffer[32];
    auto result = std::to_chars(buffer, buffer + sizeof(buffer), sequence);
    if (result.ec != std::errc()) {
        return;
    }
    const std::size_t size = static_cast<std::size_t>(result.ptr - buffer);
    if (size < kMinArchiveSequenceDigits) {
        output.append(kMinArchiveSequenceDigits - size, '0');
    }
    output.append(buffer, size);
}

bool expand_archive_name(std::string_view pattern, std::string_view base_name, std::uint64_t sequence,
                         std::string &output) {
    char utc[kUtcArchiveStampSize + 1]{};
    if (pattern.find("{utc}") != std::string_view::npos && !format_utc_archive_stamp(utc)) {
        return false;
    }

    output.clear();
    output.reserve(pattern.size() + base_name.size() + 32);
    for (std::size_t i = 0; i < pattern.size();) {
        if (pattern.substr(i).starts_with("{base}")) {
            output.append(base_name);
            i += 6;
        } else if (pattern.substr(i).starts_with("{utc}")) {
            output.append(utc, kUtcArchiveStampSize);
            i += 5;
        } else if (pattern.substr(i).starts_with("{seq}")) {
            append_archive_sequence(output, sequence);
            i += 5;
        } else {
            output.push_back(pattern[i++]);
        }
    }
    return true;
}

std::string make_child_path(std::string_view directory, std::string_view name) {
    if (directory == ".") {
        return std::string(name);
    }
    std::string path(directory);
    if (!path.ends_with('/')) {
        path.push_back('/');
    }
    path.append(name);
    return path;
}

int exchange_paths(const char *first, const char *second) noexcept {
#if defined(SYS_renameat2)
    return static_cast<int>(::syscall(SYS_renameat2, AT_FDCWD, first, AT_FDCWD, second, RENAME_EXCHANGE));
#else
    errno = ENOSYS;
    return -1;
#endif
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
            .rotations = rotations_.load(std::memory_order_relaxed),
            .rotation_errors = rotation_errors_.load(std::memory_order_relaxed),
            .retention_errors = retention_errors_.load(std::memory_order_relaxed),
            .active_file_bytes = active_file_bytes_.load(std::memory_order_relaxed),
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

void Appender::record_rotation() noexcept { rotations_.fetch_add(1, std::memory_order_relaxed); }

void Appender::record_rotation_error() noexcept {
    rotation_errors_.fetch_add(1, std::memory_order_relaxed);
    report_raw_error(last_diagnostic_second_, "fiber logging rotation failed\n");
}

void Appender::record_retention_error() noexcept {
    retention_errors_.fetch_add(1, std::memory_order_relaxed);
    report_raw_error(last_diagnostic_second_, "fiber logging retention cleanup failed\n");
}

void Appender::set_active_file_bytes(std::uint64_t bytes) noexcept {
    active_file_bytes_.store(bytes, std::memory_order_relaxed);
}

ConsoleAppender::ConsoleAppender(AppenderId id, ConsoleAppenderOptions options) noexcept :
    Appender(id, std::move(options.name), options.min_level, options.max_level),
    fd_(options.stream == ConsoleStream::Stdout ? STDOUT_FILENO : STDERR_FILENO) {}

void ConsoleAppender::append(FormattedLogLine line, LogContext &) noexcept {
    const ssize_t written = ::write(fd_, line.bytes.data(), line.bytes.size());
    if (written != static_cast<ssize_t>(line.bytes.size())) {
        record_write_error(1);
        return;
    }
    record_write(line.bytes.size(), 1);
}

void ConsoleAppender::flush(LogContext &) noexcept {}

void ConsoleAppender::flush_due(LogContext &, std::chrono::steady_clock::time_point) noexcept {}

bool ConsoleAppender::reopen() noexcept { return true; }

FileAppender::FileAppender(AppenderId id, FileAppenderOptions options, std::uint16_t buffer_slot) noexcept :
    Appender(id, std::move(options.name), options.min_level, options.max_level), path_(std::move(options.path)),
    file_mode_(options.file_mode), buffer_size_(options.buffer_size), flush_interval_(options.flush_interval),
    buffer_slot_(buffer_slot), rotation_(std::move(options.rotation)) {}

FileAppender::~FileAppender() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

bool FileAppender::open_file(int &system_error) noexcept {
    int flags = O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC;
    if (rotation_) {
        flags |= O_NOFOLLOW;
    }
    fd_ = ::open(path_.c_str(), flags, file_mode_);
    if (fd_ < 0) {
        system_error = errno;
        return false;
    }

    struct stat file_stat{};
    if (::fstat(fd_, &file_stat) < 0) {
        system_error = errno;
        return false;
    }
    if (rotation_ && !S_ISREG(file_stat.st_mode)) {
        system_error = EINVAL;
        return false;
    }
    active_bytes_ = file_stat.st_size > 0 ? static_cast<std::uint64_t>(file_stat.st_size) : 0;
    set_active_file_bytes(active_bytes_);
    if (rotation_ && !initialize_rotation(system_error)) {
        return false;
    }
    system_error = 0;
    return true;
}

bool FileAppender::initialize_rotation(int &system_error) noexcept {
    const std::size_t slash = path_.find_last_of('/');
    if (slash == std::string::npos) {
        directory_path_ = ".";
        base_name_ = path_;
    } else {
        directory_path_ = slash == 0 ? "/" : path_.substr(0, slash);
        base_name_ = path_.substr(slash + 1);
    }

    DIR *directory = ::opendir(directory_path_.c_str());
    if (!directory) {
        system_error = errno;
        return false;
    }

    archives_.clear();
    archives_.reserve(static_cast<std::size_t>(rotation_->max_archives) + 1);
    std::uint64_t max_sequence = 0;
    int scan_error = 0;
    for (;;) {
        errno = 0;
        dirent *entry = ::readdir(directory);
        if (!entry) {
            scan_error = errno;
            break;
        }
        std::uint64_t sequence = 0;
        if (!match_archive_name(rotation_->archive_name, entry->d_name, base_name_, sequence)) {
            continue;
        }
        struct stat archive_stat{};
        if (::fstatat(::dirfd(directory), entry->d_name, &archive_stat, AT_SYMLINK_NOFOLLOW) < 0 ||
            !S_ISREG(archive_stat.st_mode)) {
            continue;
        }

        max_sequence = std::max(max_sequence, sequence);
        std::string archive_path = make_child_path(directory_path_, entry->d_name);
        if (archive_stat.st_size == 0) {
            if (::unlinkat(::dirfd(directory), entry->d_name, 0) == 0 || errno == ENOENT) {
                continue;
            }
            record_retention_error();
        }
        archives_.push_back({.sequence = sequence, .path = std::move(archive_path)});
    }
    ::closedir(directory);
    if (scan_error != 0) {
        system_error = scan_error;
        return false;
    }
    if (max_sequence == std::numeric_limits<std::uint64_t>::max()) {
        system_error = EOVERFLOW;
        return false;
    }
    next_archive_sequence_ = max_sequence + 1;

    std::sort(archives_.begin(), archives_.end(),
              [](const ArchiveEntry &lhs, const ArchiveEntry &rhs) { return lhs.sequence < rhs.sequence; });
    for (std::size_t index = 0; archives_.size() > rotation_->max_archives && index < archives_.size();) {
        if (::unlink(archives_[index].path.c_str()) == 0 || errno == ENOENT) {
            archives_.erase(archives_.begin() + static_cast<std::ptrdiff_t>(index));
        } else {
            record_retention_error();
            ++index;
        }
    }
    return true;
}

bool FileAppender::should_rotate_locked(std::size_t incoming_size) const noexcept {
    if (!rotation_ || active_bytes_ == 0) {
        return false;
    }
    return active_bytes_ >= rotation_->max_file_size || incoming_size > rotation_->max_file_size - active_bytes_;
}

bool FileAppender::rotate_locked() noexcept {
    if (!rotation_ || next_archive_sequence_ == 0) {
        record_rotation_error();
        return false;
    }

    int last_error = EEXIST;
    std::uint64_t sequence = next_archive_sequence_;
    for (unsigned attempt = 0; attempt < kMaxArchiveNameAttempts; ++attempt) {
        std::string archive_name;
        if (!expand_archive_name(rotation_->archive_name, base_name_, sequence, archive_name)) {
            last_error = EINVAL;
            break;
        }
        std::string archive_path = make_child_path(directory_path_, archive_name);
        const int replacement =
                ::open(archive_path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_EXCL | O_CLOEXEC, file_mode_);
        if (replacement < 0) {
            last_error = errno;
            if (last_error == EEXIST && sequence != std::numeric_limits<std::uint64_t>::max()) {
                ++sequence;
                continue;
            }
            break;
        }

        if (exchange_paths(path_.c_str(), archive_path.c_str()) < 0) {
            last_error = errno;
            ::close(replacement);
            if (::unlink(archive_path.c_str()) < 0 && errno != ENOENT) {
                record_retention_error();
            }
            break;
        }

        const int previous = fd_;
        fd_ = replacement;
        ::close(previous);
        active_bytes_ = 0;
        set_active_file_bytes(0);
        archives_.push_back({.sequence = sequence, .path = std::move(archive_path)});
        next_archive_sequence_ = sequence == std::numeric_limits<std::uint64_t>::max() ? 0 : sequence + 1;
        record_rotation();
        rotation_retry_after_ = {};
        return true;
    }

    errno = last_error;
    record_rotation_error();
    return false;
}

void FileAppender::select_cleanup_locked(std::uint64_t &sequence, std::string &path) noexcept {
    if (!rotation_ || archives_.size() <= rotation_->max_archives || cleanup_in_progress_sequence_ != 0) {
        return;
    }
    const auto now = current_steady_time();
    if (now < cleanup_retry_after_) {
        return;
    }
    cleanup_in_progress_sequence_ = archives_.front().sequence;
    sequence = archives_.front().sequence;
    path = archives_.front().path;
}

void FileAppender::finish_cleanup(std::uint64_t sequence, std::string path) noexcept {
    const bool success = ::unlink(path.c_str()) == 0 || errno == ENOENT;
    {
        std::lock_guard<std::mutex> guard(io_mutex_);
        if (success) {
            auto it = std::find_if(archives_.begin(), archives_.end(),
                                   [sequence](const ArchiveEntry &entry) { return entry.sequence == sequence; });
            if (it != archives_.end()) {
                archives_.erase(it);
            }
            cleanup_retry_after_ = {};
        } else {
            cleanup_retry_after_ = current_steady_time() + kRotationRetryDelay;
        }
        if (cleanup_in_progress_sequence_ == sequence) {
            cleanup_in_progress_sequence_ = 0;
        }
    }
    if (!success) {
        record_retention_error();
    }
}

void FileAppender::write_bytes_locked(const char *data, std::size_t size, std::uint64_t records) noexcept {
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t written = ::write(fd_, data + offset, size - offset);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        break;
    }

    if (offset > std::numeric_limits<std::uint64_t>::max() - active_bytes_) {
        active_bytes_ = std::numeric_limits<std::uint64_t>::max();
    } else {
        active_bytes_ += offset;
    }
    set_active_file_bytes(active_bytes_);
    if (offset != size) {
        if (offset > 0) {
            record_write(offset, 0);
        }
        record_write_error(records);
        return;
    }
    record_write(size, records);
}

void FileAppender::write_bytes(const char *data, std::size_t size, std::uint64_t records) noexcept {
    std::uint64_t cleanup_sequence = 0;
    std::string cleanup_path;
    {
        std::lock_guard<std::mutex> guard(io_mutex_);
        if (should_rotate_locked(size)) {
            const auto now = current_steady_time();
            if (now >= rotation_retry_after_ && !rotate_locked()) {
                rotation_retry_after_ = now + kRotationRetryDelay;
            }
        }
        write_bytes_locked(data, size, records);
        select_cleanup_locked(cleanup_sequence, cleanup_path);
    }
    if (cleanup_sequence != 0) {
        finish_cleanup(cleanup_sequence, std::move(cleanup_path));
    }
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

void FileAppender::append(FormattedLogLine line, LogContext &context) noexcept {
    const std::size_t size = line.bytes.size();
    if (!buffered()) {
        write_bytes(line.bytes.data(), size, 1);
        return;
    }

    LogBuffer *buffer = context.buffer_at(buffer_slot_);
    if (!buffer) {
        write_bytes(line.bytes.data(), size, 1);
        return;
    }
    if (!buffer->data) {
        if (buffer->owner == this && buffer->capacity == 0) {
            write_bytes(line.bytes.data(), size, 1);
            return;
        }
        buffer->data = static_cast<char *>(std::malloc(buffer_size_));
        if (!buffer->data) {
            buffer->owner = this;
            buffer->capacity = 0;
            write_bytes(line.bytes.data(), size, 1);
            return;
        }
        buffer->owner = this;
        buffer->capacity = buffer_size_;
    }
    if (buffer->owner != this) {
        write_bytes(line.bytes.data(), size, 1);
        return;
    }

    const auto now = current_steady_time();
    if (buffer->size > 0 && now >= buffer->flush_at) {
        flush_buffer(*buffer);
    }
    if (size > buffer->capacity) {
        write_bytes(line.bytes.data(), size, 1);
        return;
    }
    if (size > buffer->capacity - buffer->size) {
        flush_buffer(*buffer);
    }
    if (buffer->size == 0) {
        buffer->flush_at = now + flush_interval_;
    }
    std::memcpy(buffer->data + buffer->size, line.bytes.data(), size);
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
    std::lock_guard<std::mutex> guard(io_mutex_);
    int flags = O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC;
    if (rotation_) {
        flags |= O_NOFOLLOW;
    }
    const int replacement = ::open(path_.c_str(), flags, file_mode_);
    if (replacement < 0) {
        record_reopen_error();
        return false;
    }

    struct stat file_stat{};
    if (::fstat(replacement, &file_stat) < 0) {
        ::close(replacement);
        record_reopen_error();
        return false;
    }
    if (rotation_ && !S_ISREG(file_stat.st_mode)) {
        ::close(replacement);
        record_reopen_error();
        return false;
    }
    const int previous = fd_;
    fd_ = replacement;
    ::close(previous);
    active_bytes_ = file_stat.st_size > 0 ? static_cast<std::uint64_t>(file_stat.st_size) : 0;
    set_active_file_bytes(active_bytes_);
    return true;
}

} // namespace fiber::log
