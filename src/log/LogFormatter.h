#ifndef FIBER_LOG_LOG_FORMATTER_H
#define FIBER_LOG_LOG_FORMATTER_H

#include <cstddef>

#include "LogEvent.h"

namespace fiber::log::detail {

[[nodiscard]] std::size_t format_log_event(const LogEvent &event, char *data, std::size_t capacity) noexcept;

} // namespace fiber::log::detail

#endif // FIBER_LOG_LOG_FORMATTER_H
