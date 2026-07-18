#ifndef FIBER_LOG_LOG_LINE_H
#define FIBER_LOG_LOG_LINE_H

#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>

#include "Logger.h"

namespace fiber::log {

struct QuotedLogValue {
    std::string_view value;
};

[[nodiscard]] inline QuotedLogValue quoted(std::string_view value) noexcept { return {value}; }

class LogLine {
public:
    static constexpr std::size_t kMessageCapacity = 8192;

    LogLine(const Logger &logger, LogLevel level, const char *file, std::uint32_t line, const char *function) noexcept;
    ~LogLine();

    LogLine(const LogLine &) = delete;
    LogLine &operator=(const LogLine &) = delete;
    LogLine(LogLine &&) = delete;
    LogLine &operator=(LogLine &&) = delete;

    LogLine &operator<<(std::string_view value) noexcept;
    LogLine &operator<<(const std::string &value) noexcept { return *this << std::string_view(value); }
    LogLine &operator<<(const char *value) noexcept;
    LogLine &operator<<(char value) noexcept;
    LogLine &operator<<(bool value) noexcept;
    LogLine &operator<<(const void *value) noexcept;
    LogLine &operator<<(std::nullptr_t) noexcept;
    LogLine &operator<<(QuotedLogValue value) noexcept;

    template<std::integral T>
        requires(!std::same_as<std::remove_cv_t<T>, bool> && !std::same_as<std::remove_cv_t<T>, char> &&
                 !std::same_as<std::remove_cv_t<T>, signed char> && !std::same_as<std::remove_cv_t<T>, unsigned char>)
    LogLine &operator<<(T value) noexcept {
        char buffer[std::numeric_limits<T>::digits10 + 4];
        auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
        if (result.ec == std::errc()) {
            append_raw(std::string_view(buffer, static_cast<std::size_t>(result.ptr - buffer)));
        } else {
            append_raw("<format-error>");
        }
        return *this;
    }

    LogLine &operator<<(signed char value) noexcept { return *this << static_cast<int>(value); }
    LogLine &operator<<(unsigned char value) noexcept { return *this << static_cast<unsigned int>(value); }

    template<std::floating_point T>
    LogLine &operator<<(T value) noexcept {
        char buffer[64];
        auto result = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general,
                                    std::numeric_limits<T>::max_digits10);
        if (result.ec == std::errc()) {
            append_raw(std::string_view(buffer, static_cast<std::size_t>(result.ptr - buffer)));
        } else {
            append_raw("<format-error>");
        }
        return *this;
    }

    template<typename T>
        requires std::is_enum_v<T>
    LogLine &operator<<(T value) noexcept {
        return *this << static_cast<std::underlying_type_t<T>>(value);
    }

private:
    void append_raw(std::string_view value) noexcept;
    void append_escaped(std::string_view value) noexcept;
    void append_quoted(std::string_view value) noexcept;
    void finish_message() noexcept;

    const Logger &logger_;
    LogLevel level_;
    const char *file_;
    const char *function_;
    std::uint32_t line_;
    std::uint64_t timestamp_us_ = 0;
    std::uint32_t thread_id_ = 0;
    char message_[kMessageCapacity]{};
    std::size_t message_size_ = 0;
    bool truncated_ = false;
};

class NullLogLine {
public:
    template<typename T>
    constexpr const NullLogLine &operator<<(T &&) const noexcept {
        return *this;
    }
};

} // namespace fiber::log

#endif // FIBER_LOG_LOG_LINE_H
