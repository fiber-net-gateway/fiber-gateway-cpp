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

std::size_t archive_name_max_size(std::string_view pattern, std::string_view base_name) noexcept {
    constexpr std::size_t kUtcSize = 16;
    constexpr std::size_t kMaxSequenceSize = 20;
    std::size_t size = 0;
    for (std::size_t i = 0; i < pattern.size();) {
        if (pattern.substr(i).starts_with("{base}")) {
            size += base_name.size();
            i += 6;
        } else if (pattern.substr(i).starts_with("{utc}")) {
            size += kUtcSize;
            i += 5;
        } else if (pattern.substr(i).starts_with("{seq}")) {
            size += kMaxSequenceSize;
            i += 5;
        } else {
            ++size;
            ++i;
        }
    }
    return size;
}

std::string_view path_basename(std::string_view path) noexcept {
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
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

bool valid_archive_name_pattern(std::string_view pattern) noexcept {
    if (pattern.empty() || pattern.size() > 255 || pattern.find('/') != std::string_view::npos ||
        pattern.find('\\') != std::string_view::npos || pattern.find("..") != std::string_view::npos) {
        return false;
    }

    unsigned base_count = 0;
    unsigned utc_count = 0;
    unsigned sequence_count = 0;
    for (std::size_t i = 0; i < pattern.size();) {
        if (pattern.substr(i).starts_with("{base}")) {
            ++base_count;
            i += 6;
            continue;
        }
        if (pattern.substr(i).starts_with("{utc}")) {
            ++utc_count;
            i += 5;
            continue;
        }
        if (pattern.substr(i).starts_with("{seq}")) {
            ++sequence_count;
            i += 5;
            continue;
        }
        const unsigned char ch = static_cast<unsigned char>(pattern[i]);
        const bool alpha = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
        const bool digit = ch >= '0' && ch <= '9';
        if (!alpha && !digit && ch != '.' && ch != '_' && ch != '-') {
            return false;
        }
        ++i;
    }
    return base_count == 1 && utc_count <= 1 && sequence_count == 1;
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
    if (options.rotation) {
        const FileRotationOptions &rotation = *options.rotation;
        if (rotation.max_file_size == 0 || rotation.max_file_size < options.buffer_size || rotation.max_archives == 0 ||
            rotation.max_archives > kMaxRetainedLogArchives) {
            return std::unexpected(
                    make_error(LogConfigErrorCode::InvalidRotationOptions, "invalid file rotation options"));
        }
        if (!valid_archive_name_pattern(rotation.archive_name) ||
            archive_name_max_size(rotation.archive_name, path_basename(options.path)) > 255) {
            return std::unexpected(make_error(LogConfigErrorCode::InvalidArchiveName, "invalid archive name pattern"));
        }
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

LogConfigResult<void> LogConfigBuilder::request_logger(std::string name) {
    if (auto result = ensure_building(); !result) {
        return result;
    }
    if (!valid_logger_name(name)) {
        return std::unexpected(make_error(LogConfigErrorCode::InvalidName, "invalid requested logger name"));
    }
    if (std::find(config_.requested_loggers_.begin(), config_.requested_loggers_.end(), name) ==
        config_.requested_loggers_.end()) {
        config_.requested_loggers_.push_back(std::move(name));
    }
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

LogConfigResult<void> LogConfigBuilder::set_async_options(AsyncLogOptions options) {
    if (auto result = ensure_building(); !result) {
        return result;
    }
    if (options.backlog_capacity == 0 || options.backlog_capacity == std::numeric_limits<std::size_t>::max()) {
        return std::unexpected(make_error(LogConfigErrorCode::InvalidBufferOptions,
                                          "log backlog capacity must be positive and fit the admission state"));
    }
    if (options.full_policy != LogQueueFullPolicy::Block && options.full_policy != LogQueueFullPolicy::DropNewest) {
        return std::unexpected(make_error(LogConfigErrorCode::InvalidBufferOptions, "invalid log queue full policy"));
    }
    config_.async_ = options;
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
