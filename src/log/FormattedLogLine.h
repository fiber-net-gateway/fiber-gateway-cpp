#ifndef FIBER_LOG_FORMATTED_LOG_LINE_H
#define FIBER_LOG_FORMATTED_LOG_LINE_H

#include <string_view>

namespace fiber::log {

struct FormattedLogLine {
    // A complete record including its trailing newline. The view remains valid
    // only for the duration of the synchronous Appender::append() call.
    std::string_view bytes;
};

} // namespace fiber::log

#endif // FIBER_LOG_FORMATTED_LOG_LINE_H
