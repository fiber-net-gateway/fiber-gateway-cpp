#include "LogFormatter.h"

#include <charconv>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <string_view>

namespace fiber::log::detail {
namespace {

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

struct TimestampCache {
    std::time_t seconds = 0;
    std::size_t date_size = 0;
    std::size_t zone_size = 0;
    char date[32];
    char zone[8];
    bool initialized = false;
    bool local_time_valid = false;
};

TimestampCache &timestamp_cache() noexcept {
    static thread_local TimestampCache cache;
    return cache;
}

void refresh_timestamp_cache(TimestampCache &cache, std::time_t seconds) noexcept {
    cache.seconds = seconds;
    cache.date_size = 0;
    cache.zone_size = 0;
    cache.initialized = true;

    std::tm local;
    cache.local_time_valid = ::localtime_r(&seconds, &local) != nullptr;
    if (!cache.local_time_valid) {
        return;
    }

    cache.date_size = std::strftime(cache.date, sizeof(cache.date), "%Y-%m-%dT%H:%M:%S", &local);

    char raw_zone[8];
    const std::size_t raw_zone_size = std::strftime(raw_zone, sizeof(raw_zone), "%z", &local);
    if (raw_zone_size == 5) {
        std::memcpy(cache.zone, raw_zone, 3);
        cache.zone[3] = ':';
        std::memcpy(cache.zone + 4, raw_zone + 3, 2);
        cache.zone_size = 6;
    } else if (raw_zone_size <= sizeof(cache.zone)) {
        std::memcpy(cache.zone, raw_zone, raw_zone_size);
        cache.zone_size = raw_zone_size;
    }
}

std::string_view source_basename(std::string_view file) noexcept {
    const std::size_t slash = file.find_last_of("/\\");
    return slash == std::string_view::npos ? file : file.substr(slash + 1);
}

} // namespace

std::size_t format_log_event(const LogEvent &event, char *data, std::size_t capacity) noexcept {
    if (capacity == 0) {
        return 0;
    }
    FixedWriter writer(data, capacity - 1);

    const std::time_t seconds = static_cast<std::time_t>(event.timestamp_us / 1000000);
    const auto micros = static_cast<std::uint32_t>(event.timestamp_us % 1000000);
    TimestampCache &cache = timestamp_cache();
    if (!cache.initialized || cache.seconds != seconds) {
        refresh_timestamp_cache(cache, seconds);
    }
    if (cache.local_time_valid) {
        writer.append(std::string_view(cache.date, cache.date_size));
        writer.append('.');
        writer.append_six_digits(micros);
        writer.append(std::string_view(cache.zone, cache.zone_size));
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

} // namespace fiber::log::detail
