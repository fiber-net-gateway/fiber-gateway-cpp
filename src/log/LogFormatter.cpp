#include "LogFormatter.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace fiber::log {
namespace {

std::string_view source_basename(std::string_view file) noexcept {
    const std::size_t slash = file.find_last_of("/\\");
    return slash == std::string_view::npos ? file : file.substr(slash + 1);
}

} // namespace

LogFormatter::~LogFormatter() {
    if (prefix_ != inline_prefix_) {
        std::free(prefix_);
    }
}

bool LogFormatter::ensure_capacity(std::size_t required) noexcept {
    if (required <= prefix_capacity_) {
        return true;
    }
    std::size_t capacity = prefix_capacity_;
    while (capacity < required) {
        if (capacity > std::numeric_limits<std::size_t>::max() / 2) {
            capacity = required;
            break;
        }
        capacity *= 2;
    }

    if (prefix_ == inline_prefix_) {
        char *replacement = static_cast<char *>(std::malloc(capacity));
        if (!replacement) {
            return false;
        }
        std::memcpy(replacement, inline_prefix_, prefix_size_);
        prefix_ = replacement;
    } else {
        char *replacement = static_cast<char *>(std::realloc(prefix_, capacity));
        if (!replacement) {
            return false;
        }
        prefix_ = replacement;
    }
    prefix_capacity_ = capacity;
    return true;
}

bool LogFormatter::append(std::string_view value) noexcept {
    if (prefix_size_ > std::numeric_limits<std::size_t>::max() - value.size() ||
        !ensure_capacity(prefix_size_ + value.size())) {
        return false;
    }
    if (!value.empty()) {
        std::memcpy(prefix_ + prefix_size_, value.data(), value.size());
        prefix_size_ += value.size();
    }
    return true;
}

bool LogFormatter::append(char value) noexcept {
    if (prefix_size_ == std::numeric_limits<std::size_t>::max() || !ensure_capacity(prefix_size_ + 1)) {
        return false;
    }
    prefix_[prefix_size_++] = value;
    return true;
}

bool LogFormatter::append_six_digits(std::uint32_t value) noexcept {
    char buffer[6];
    for (std::size_t i = sizeof(buffer); i > 0; --i) {
        buffer[i - 1] = static_cast<char>('0' + value % 10);
        value /= 10;
    }
    return append(std::string_view(buffer, sizeof(buffer)));
}

void LogFormatter::refresh_timestamp_cache(std::time_t seconds) noexcept {
    cached_seconds_ = seconds;
    cached_date_size_ = 0;
    cached_zone_size_ = 0;
    timestamp_initialized_ = true;

    std::tm local{};
    local_time_valid_ = ::localtime_r(&seconds, &local) != nullptr;
    if (!local_time_valid_) {
        return;
    }

    cached_date_size_ = std::strftime(cached_date_, sizeof(cached_date_), "%Y-%m-%dT%H:%M:%S", &local);
    char raw_zone[8];
    const std::size_t raw_zone_size = std::strftime(raw_zone, sizeof(raw_zone), "%z", &local);
    if (raw_zone_size == 5) {
        std::memcpy(cached_zone_, raw_zone, 3);
        cached_zone_[3] = ':';
        std::memcpy(cached_zone_ + 4, raw_zone + 3, 2);
        cached_zone_size_ = 6;
    } else if (raw_zone_size <= sizeof(cached_zone_)) {
        std::memcpy(cached_zone_, raw_zone, raw_zone_size);
        cached_zone_size_ = raw_zone_size;
    }
}

bool LogFormatter::format(const OwnedLogRecord &record, FormattedLogRecord &output) noexcept {
    prefix_size_ = 0;
    const std::time_t seconds = static_cast<std::time_t>(record.timestamp_us() / 1000000);
    const auto micros = static_cast<std::uint32_t>(record.timestamp_us() % 1000000);
    if (!timestamp_initialized_ || cached_seconds_ != seconds) {
        refresh_timestamp_cache(seconds);
    }

    bool success = true;
    if (local_time_valid_) {
        success = append(std::string_view(cached_date_, cached_date_size_)) && append('.') &&
                  append_six_digits(micros) && append(std::string_view(cached_zone_, cached_zone_size_));
    } else {
        success = append_integer(record.timestamp_us());
    }
    success = success && append(' ') && append(log_level_name(record.level())) && append(" [worker=") &&
              append_integer(record.thread_id()) && append("] ") && append(record.logger_name()) && append(' ') &&
              append(source_basename(record.file())) && append(':') && append_integer(record.line()) && append(' ');
    if (!success || record.message_size() == std::numeric_limits<std::size_t>::max() ||
        prefix_size_ > std::numeric_limits<std::size_t>::max() - record.message_size() - 1) {
        output = {};
        return false;
    }

    output.record_ = &record;
    output.prefix_ = prefix_;
    output.prefix_size_ = prefix_size_;
    output.total_size_ = prefix_size_ + record.message_size() + 1;
    return true;
}

} // namespace fiber::log
