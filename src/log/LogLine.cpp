#include "LogLine.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <sys/syscall.h>
#include <unistd.h>

#include "../event/EventLoop.h"
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
#if defined(SYS_gettid)
    return static_cast<std::uint32_t>(::syscall(SYS_gettid));
#else
    return static_cast<std::uint32_t>(::getpid());
#endif
}

char hex_digit(unsigned value) noexcept { return static_cast<char>(value < 10 ? '0' + value : 'A' + value - 10); }

} // namespace

LogLine::LogLine(const Logger &logger, LogLevel level, const char *file, std::uint32_t line,
                 const char *function) noexcept :
    logger_(logger), level_(level), file_(file), function_(function), line_(line),
    timestamp_us_(current_timestamp_us()), thread_id_(current_thread_id()) {}

LogLine::~LogLine() {
    finish_message();
    LogEvent event{
            .logger_name = logger_.name(),
            .message = std::string_view(message_, message_size_),
            .file = file_ ? std::string_view(file_) : std::string_view(),
            .function = function_ ? std::string_view(function_) : std::string_view(),
            .level = level_,
            .line = line_,
            .timestamp_us = timestamp_us_,
            .thread_id = thread_id_,
    };
    auto &manager = LoggerManager::global();
    logger_.dispatch(event, manager.current_context());
}

void LogLine::append_raw(std::string_view value) noexcept {
    const std::size_t remaining = kMessageCapacity - message_size_;
    const std::size_t copy = std::min(remaining, value.size());
    if (copy > 0) {
        std::memcpy(message_ + message_size_, value.data(), copy);
        message_size_ += copy;
    }
    if (copy != value.size()) {
        truncated_ = true;
    }
}

void LogLine::append_escaped(std::string_view value) noexcept {
    for (unsigned char ch: value) {
        switch (ch) {
            case '\n':
                append_raw("\\n");
                break;
            case '\r':
                append_raw("\\r");
                break;
            case '\t':
                append_raw("\\t");
                break;
            default:
                if (ch < 0x20 || ch == 0x7f) {
                    char escaped[] = {'\\', 'x', hex_digit(ch >> 4), hex_digit(ch & 0xf)};
                    append_raw(std::string_view(escaped, sizeof(escaped)));
                } else {
                    append_raw(std::string_view(reinterpret_cast<const char *>(&ch), 1));
                }
                break;
        }
        if (truncated_) {
            return;
        }
    }
}

void LogLine::append_quoted(std::string_view value) noexcept {
    append_raw("\"");
    for (unsigned char ch: value) {
        if (ch == '\\' || ch == '"') {
            char escaped[] = {'\\', static_cast<char>(ch)};
            append_raw(std::string_view(escaped, sizeof(escaped)));
        } else {
            append_escaped(std::string_view(reinterpret_cast<const char *>(&ch), 1));
        }
        if (truncated_) {
            return;
        }
    }
    append_raw("\"");
}

void LogLine::finish_message() noexcept {
    if (!truncated_) {
        return;
    }
    constexpr std::string_view marker = " ... <truncated>";
    const std::size_t prefix_size = kMessageCapacity - marker.size();
    if (message_size_ > prefix_size) {
        message_size_ = prefix_size;
    }
    append_raw(marker);
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

} // namespace fiber::log
