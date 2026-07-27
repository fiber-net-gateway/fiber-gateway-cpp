#include "LogLine.h"

#include <chrono>
#include <sys/syscall.h>
#include <unistd.h>

#include "../event/EventLoop.h"
#include "LogRecord.h"
#include "LoggerManager.h"

namespace fiber::log {
namespace {

std::uint64_t current_timestamp_us() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

std::uint32_t current_thread_id() noexcept {
    if (auto *loop = fiber::event::EventLoop::current_or_null(); loop && loop->has_group_index()) {
        return static_cast<std::uint32_t>(loop->group_index());
    }
    static thread_local const std::uint32_t cached_thread_id = []() noexcept {
#if defined(SYS_gettid)
        return static_cast<std::uint32_t>(::syscall(SYS_gettid));
#else
        return static_cast<std::uint32_t>(::getpid());
#endif
    }();
    return cached_thread_id;
}

char hex_digit(unsigned value) noexcept { return static_cast<char>(value < 10 ? '0' + value : 'A' + value - 10); }

} // namespace

LogLine::LogLine(const Logger &logger, LogLevel level, const char *file, std::uint32_t line,
                 const char *function) noexcept {
    const LevelTargets &targets = logger.targets(level);
    record_ = OwnedLogRecord::create(logger.name(), targets.first, targets.count, level, file, line, function,
                                     current_timestamp_us(), current_thread_id());
}

LogLine::~LogLine() noexcept {
    auto &manager = LoggerManager::global();
    if (!record_) {
        manager.record_allocation_failure();
        return;
    }
    (void) manager.submit(record_);
}

void LogLine::append_raw(std::string_view value) noexcept {
    if (record_) {
        (void) record_->append(value);
    }
}

void LogLine::append_escaped(std::string_view value) noexcept {
    std::size_t plain_start = 0;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const auto ch = static_cast<unsigned char>(value[index]);
        std::string_view escaped;
        char escaped_control[4];
        switch (ch) {
            case '\n':
                escaped = "\\n";
                break;
            case '\r':
                escaped = "\\r";
                break;
            case '\t':
                escaped = "\\t";
                break;
            default:
                if (ch < 0x20 || ch == 0x7f) {
                    escaped_control[0] = '\\';
                    escaped_control[1] = 'x';
                    escaped_control[2] = hex_digit(ch >> 4);
                    escaped_control[3] = hex_digit(ch & 0xf);
                    escaped = std::string_view(escaped_control, sizeof(escaped_control));
                } else {
                    continue;
                }
                break;
        }
        append_raw(value.substr(plain_start, index - plain_start));
        append_raw(escaped);
        plain_start = index + 1;
        if (!record_ || record_->failed()) {
            return;
        }
    }
    append_raw(value.substr(plain_start));
}

void LogLine::append_quoted(std::string_view value) noexcept {
    append_raw("\"");
    std::size_t plain_start = 0;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const auto ch = static_cast<unsigned char>(value[index]);
        if (ch != '\\' && ch != '"' && ch >= 0x20 && ch != 0x7f) {
            continue;
        }
        append_escaped(value.substr(plain_start, index - plain_start));
        if (ch == '\\' || ch == '"') {
            const char escaped[] = {'\\', static_cast<char>(ch)};
            append_raw(std::string_view(escaped, sizeof(escaped)));
        } else {
            append_escaped(value.substr(index, 1));
        }
        plain_start = index + 1;
        if (!record_ || record_->failed()) {
            return;
        }
    }
    append_escaped(value.substr(plain_start));
    append_raw("\"");
}

LogLine &LogLine::operator<<(std::string_view value) noexcept {
    append_escaped(value);
    return *this;
}

LogLine &LogLine::operator<<(const char *value) noexcept {
    return *this << (value ? std::string_view(value) : std::string_view("(null)"));
}

LogLine &LogLine::operator<<(char value) noexcept {
    append_escaped(std::string_view(&value, 1));
    return *this;
}

LogLine &LogLine::operator<<(bool value) noexcept {
    append_raw(value ? "true" : "false");
    return *this;
}

LogLine &LogLine::operator<<(const void *value) noexcept {
    if (!value) {
        append_raw("nullptr");
        return *this;
    }
    append_raw("0x");
    char buffer[2 * sizeof(std::uintptr_t)];
    auto result = std::to_chars(buffer, buffer + sizeof(buffer), reinterpret_cast<std::uintptr_t>(value), 16);
    if (result.ec == std::errc()) {
        append_raw(std::string_view(buffer, static_cast<std::size_t>(result.ptr - buffer)));
    } else {
        append_raw("<format-error>");
    }
    return *this;
}

LogLine &LogLine::operator<<(std::nullptr_t) noexcept { return *this << static_cast<const void *>(nullptr); }

LogLine &LogLine::operator<<(QuotedLogValue value) noexcept {
    append_quoted(value.value);
    return *this;
}

bool log_complete_message(const Logger &logger, LogLevel level, const char *file, std::uint32_t line,
                          const char *function, std::string_view message) noexcept {
    if (!logger.enabled(level)) {
        return true;
    }
    const LevelTargets &targets = logger.targets(level);
    OwnedLogRecord *record = OwnedLogRecord::create(logger.name(), targets.first, targets.count, level, file, line,
                                                    function, current_timestamp_us(), current_thread_id());
    if (!record) {
        LoggerManager::global().record_allocation_failure();
        return false;
    }
    if (!record->append(message)) {
        (void) LoggerManager::global().submit(record);
        return false;
    }
    return LoggerManager::global().submit(record);
}

} // namespace fiber::log
