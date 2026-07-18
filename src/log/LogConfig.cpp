#include "LogConfig.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace fiber::log {
namespace {

LogConfigError make_error(LogConfigErrorCode code, std::string message) {
    return LogConfigError{.code = code, .message = std::move(message)};
}

bool valid_level_range(LogLevel min_level, LogLevel max_level) noexcept {
    return valid_log_level(min_level) && valid_log_level(max_level) && level_index(min_level) <= level_index(max_level);
}

} // namespace

bool valid_logger_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > kMaxLoggerNameLength || name.front() == '.' || name.back() == '.') {
        return false;
    }
    bool previous_dot = false;
    for (char ch: name) {
        if (ch == '.') {
            if (previous_dot) {
                return false;
            }
            previous_dot = true;
            continue;
        }
        previous_dot = false;
        const bool alpha = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
        const bool digit = ch >= '0' && ch <= '9';
        if (!alpha && !digit && ch != '_' && ch != '-') {
            return false;
        }
    }
    return true;
}

LogConfigResult<void> LogConfigBuilder::ensure_building() const {
    if (finished_) {
        return std::unexpected(make_error(LogConfigErrorCode::AlreadyFinished, "log config builder is finished"));
    }
    return {};
}

LogConfigResult<void> LogConfigBuilder::validate_appender_name(std::string_view name) const {
    if (!valid_logger_name(name)) {
        return std::unexpected(make_error(LogConfigErrorCode::InvalidName, "invalid appender name"));
    }
    for (const auto &definition: config_.appenders_) {
        if (definition.name() == name) {
            return std::unexpected(make_error(LogConfigErrorCode::DuplicateAppender, "duplicate appender name"));
        }
    }
    return {};
}

LogConfigResult<void> LogConfigBuilder::validate_appender_ids(const std::vector<AppenderId> &appenders) const {
    for (AppenderId id: appenders) {
        if (id >= config_.appenders_.size()) {
            return std::unexpected(make_error(LogConfigErrorCode::UnknownAppender, "unknown appender id"));
        }
    }
    return {};
}

LogConfigResult<AppenderId> LogConfigBuilder::add_file_appender(FileAppenderOptions options) {
    if (auto result = ensure_building(); !result) {
        return std::unexpected(result.error());
    }
    if (auto result = validate_appender_name(options.name); !result) {
        return std::unexpected(result.error());
    }
    if (options.path.empty()) {
        return std::unexpected(make_error(LogConfigErrorCode::InvalidFilePath, "file appender path is empty"));
    }
    if (!valid_level_range(options.min_level, options.max_level)) {
        return std::unexpected(make_error(LogConfigErrorCode::InvalidLevelRange, "invalid file appender level range"));
    }
    if ((options.buffer_size == 0) != (options.flush_interval.count() == 0)) {
        return std::unexpected(make_error(LogConfigErrorCode::InvalidBufferOptions,
                                          "buffer_size and flush_interval must both be zero or non-zero"));
    }
    if (options.flush_interval.count() < 0) {
        return std::unexpected(make_error(LogConfigErrorCode::InvalidBufferOptions, "negative flush interval"));
    }
    if (options.buffer_size > static_cast<std::size_t>(std::numeric_limits<ssize_t>::max())) {
        return std::unexpected(make_error(LogConfigErrorCode::InvalidBufferOptions, "buffer size exceeds write limit"));
    }
    for (const auto &definition: config_.appenders_) {
        if (definition.type == LogConfig::AppenderDefinition::Type::File && definition.file.path == options.path) {
            return std::unexpected(
                    make_error(LogConfigErrorCode::DuplicateFilePath, "multiple appenders use the same file path"));
        }
    }
    if (config_.appenders_.size() >= kInvalidAppenderId) {
        return std::unexpected(make_error(LogConfigErrorCode::SizeOverflow, "too many appenders"));
    }

    const auto id = static_cast<AppenderId>(config_.appenders_.size());
    LogConfig::AppenderDefinition definition;
    definition.type = LogConfig::AppenderDefinition::Type::File;
    definition.file = std::move(options);
    config_.appenders_.push_back(std::move(definition));
    return id;
}

LogConfigResult<AppenderId> LogConfigBuilder::add_console_appender(ConsoleAppenderOptions options) {
    if (auto result = ensure_building(); !result) {
        return std::unexpected(result.error());
    }
    if (auto result = validate_appender_name(options.name); !result) {
        return std::unexpected(result.error());
    }
    if (!valid_level_range(options.min_level, options.max_level)) {
        return std::unexpected(
                make_error(LogConfigErrorCode::InvalidLevelRange, "invalid console appender level range"));
    }
    if (config_.appenders_.size() >= kInvalidAppenderId) {
        return std::unexpected(make_error(LogConfigErrorCode::SizeOverflow, "too many appenders"));
    }

    const auto id = static_cast<AppenderId>(config_.appenders_.size());
    LogConfig::AppenderDefinition definition;
    definition.type = LogConfig::AppenderDefinition::Type::Console;
    definition.console = std::move(options);
    config_.appenders_.push_back(std::move(definition));
    return id;
}

LogConfigResult<void> LogConfigBuilder::add_logger(LoggerOptions options, std::initializer_list<AppenderId> appenders) {
    return add_logger(std::move(options), std::vector<AppenderId>(appenders));
}

LogConfigResult<void> LogConfigBuilder::add_logger(LoggerOptions options, std::vector<AppenderId> appenders) {
    if (auto result = ensure_building(); !result) {
        return result;
    }
    if (!valid_logger_name(options.name)) {
        return std::unexpected(make_error(LogConfigErrorCode::InvalidName, "invalid logger name"));
    }
    if (options.level && !valid_log_level(*options.level)) {
        return std::unexpected(make_error(LogConfigErrorCode::InvalidLevelRange, "invalid logger level"));
    }
    if (auto result = validate_appender_ids(appenders); !result) {
        return result;
    }
    for (const auto &definition: config_.loggers_) {
        if (definition.options.name == options.name) {
            return std::unexpected(make_error(LogConfigErrorCode::DuplicateLogger, "duplicate logger rule"));
        }
    }
    config_.loggers_.push_back({.options = std::move(options), .appenders = std::move(appenders)});
    return {};
}

LogConfigResult<void> LogConfigBuilder::set_root_logger(RootLoggerOptions options,
                                                        std::initializer_list<AppenderId> appenders) {
    return set_root_logger(options, std::vector<AppenderId>(appenders));
}

LogConfigResult<void> LogConfigBuilder::set_root_logger(RootLoggerOptions options, std::vector<AppenderId> appenders) {
    if (auto result = ensure_building(); !result) {
        return result;
    }
    if (config_.has_root_) {
        return std::unexpected(make_error(LogConfigErrorCode::DuplicateLogger, "root logger is already configured"));
    }
    if (!valid_log_level(options.level)) {
        return std::unexpected(make_error(LogConfigErrorCode::InvalidLevelRange, "invalid root logger level"));
    }
    if (auto result = validate_appender_ids(appenders); !result) {
        return result;
    }
    config_.root_ = options;
    config_.root_appenders_ = std::move(appenders);
    config_.has_root_ = true;
    return {};
}

LogConfigResult<LogConfig> LogConfigBuilder::finish() {
    if (auto result = ensure_building(); !result) {
        return std::unexpected(result.error());
    }
    if (!config_.has_root_) {
        return std::unexpected(make_error(LogConfigErrorCode::MissingRootLogger, "root logger is not configured"));
    }
    finished_ = true;
    return std::move(config_);
}

} // namespace fiber::log
