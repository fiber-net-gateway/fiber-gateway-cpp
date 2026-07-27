#ifndef FIBER_LOG_LOG_FORMATTER_H
#define FIBER_LOG_LOG_FORMATTER_H

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string_view>

#include "FormattedLogRecord.h"

namespace fiber::log {

class LogFormatter {
public:
    LogFormatter() noexcept = default;
    ~LogFormatter();

    LogFormatter(const LogFormatter &) = delete;
    LogFormatter &operator=(const LogFormatter &) = delete;
    LogFormatter(LogFormatter &&) = delete;
    LogFormatter &operator=(LogFormatter &&) = delete;

    [[nodiscard]] bool format(const OwnedLogRecord &record, FormattedLogRecord &output) noexcept;

private:
    [[nodiscard]] bool ensure_capacity(std::size_t required) noexcept;
    [[nodiscard]] bool append(std::string_view value) noexcept;
    [[nodiscard]] bool append(char value) noexcept;

    template<typename T>
    [[nodiscard]] bool append_integer(T value) noexcept {
        char buffer[32];
        auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
        return result.ec == std::errc() &&
               append(std::string_view(buffer, static_cast<std::size_t>(result.ptr - buffer)));
    }

    [[nodiscard]] bool append_six_digits(std::uint32_t value) noexcept;
    void refresh_timestamp_cache(std::time_t seconds) noexcept;

    static constexpr std::size_t kInlinePrefixCapacity = 512;

    char inline_prefix_[kInlinePrefixCapacity];
    char *prefix_ = inline_prefix_;
    std::size_t prefix_size_ = 0;
    std::size_t prefix_capacity_ = kInlinePrefixCapacity;
    std::time_t cached_seconds_ = 0;
    std::size_t cached_date_size_ = 0;
    std::size_t cached_zone_size_ = 0;
    char cached_date_[32];
    char cached_zone_[8];
    bool timestamp_initialized_ = false;
    bool local_time_valid_ = false;
};

} // namespace fiber::log

#endif // FIBER_LOG_LOG_FORMATTER_H
