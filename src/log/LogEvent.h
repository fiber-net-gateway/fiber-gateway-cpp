#ifndef FIBER_LOG_LOG_EVENT_H
#define FIBER_LOG_LOG_EVENT_H

#include <cstdint>
#include <string_view>

#include "LogLevel.h"

namespace fiber::log {

struct LogEvent {
    std::string_view logger_name;
    std::string_view message;
    std::string_view file;
    std::string_view function;
    LogLevel level = LogLevel::Info;
    std::uint32_t line = 0;
    std::uint64_t timestamp_us = 0;
    std::uint32_t thread_id = 0;
};

} // namespace fiber::log

#endif // FIBER_LOG_LOG_EVENT_H
