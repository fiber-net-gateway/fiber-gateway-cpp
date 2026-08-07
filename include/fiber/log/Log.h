#ifndef FIBER_LOG_LOG_H
#define FIBER_LOG_LOG_H

#include "LogConfig.h"
#include "LogLine.h"
#include "LoggerManager.h"

#define FIBER_LOG_LEVEL_TRACE ::fiber::log::LogLevel::Trace
#define FIBER_LOG_LEVEL_DEBUG ::fiber::log::LogLevel::Debug
#define FIBER_LOG_LEVEL_INFO ::fiber::log::LogLevel::Info
#define FIBER_LOG_LEVEL_WARN ::fiber::log::LogLevel::Warn
#define FIBER_LOG_LEVEL_WARNING ::fiber::log::LogLevel::Warn
#define FIBER_LOG_LEVEL_ERROR ::fiber::log::LogLevel::Error
#define FIBER_LOG_LEVEL_FATAL ::fiber::log::LogLevel::Fatal
#define FIBER_LOG_LEVEL_IMPL(level) FIBER_LOG_LEVEL_##level
#define FIBER_LOG_LEVEL(level) FIBER_LOG_LEVEL_IMPL(level)

#define DEFINE_LOGGER(variable, logger_name)                                                                           \
    static ::fiber::log::LoggerHandle variable { logger_name }

#define LOG(logger_handle, level)                                                                                      \
    if (const ::fiber::log::Logger *fiber_log_logger__ = &(logger_handle).get();                                       \
        !fiber_log_logger__->enabled(FIBER_LOG_LEVEL(level))) {                                                        \
    } else                                                                                                             \
        ::fiber::log::LogLine(*fiber_log_logger__, FIBER_LOG_LEVEL(level), __FILE__, __LINE__, __func__)

#define LOG_IF(logger_handle, level, condition)                                                                        \
    if (const ::fiber::log::Logger *fiber_log_logger__ = &(logger_handle).get();                                       \
        !fiber_log_logger__->enabled(FIBER_LOG_LEVEL(level)) || !(condition)) {                                        \
    } else                                                                                                             \
        ::fiber::log::LogLine(*fiber_log_logger__, FIBER_LOG_LEVEL(level), __FILE__, __LINE__, __func__)

#if defined(NDEBUG)
#define DLOG(logger_handle, level)                                                                                     \
    if (true) {                                                                                                        \
    } else                                                                                                             \
        ::fiber::log::NullLogLine()
#else
#define DLOG(logger_handle, level) LOG(logger_handle, level)
#endif

#define VLOG(logger_handle, verbosity)                                                                                 \
    if (const ::fiber::log::Logger *fiber_log_logger__ = &(logger_handle).get();                                       \
        !fiber_log_logger__->vlog_enabled(verbosity)) {                                                                \
    } else                                                                                                             \
        ::fiber::log::LogLine(*fiber_log_logger__, ::fiber::log::LogLevel::Debug, __FILE__, __LINE__, __func__)

#endif // FIBER_LOG_LOG_H
