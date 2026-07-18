#ifndef FIBER_LOG_LOG_CONFIG_H
#define FIBER_LOG_LOG_CONFIG_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

#include "LogLevel.h"

namespace fiber::log {

using AppenderId = std::uint16_t;
inline constexpr AppenderId kInvalidAppenderId = static_cast<AppenderId>(-1);
inline constexpr std::size_t kMaxLoggerNameLength = 255;

enum class ConsoleStream : std::uint8_t {
    Stdout,
    Stderr,
};

enum class LogConfigErrorCode : std::uint8_t {
    None = 0,
    InvalidName,
    DuplicateAppender,
    DuplicateLogger,
    UnknownAppender,
    InvalidLevelRange,
    InvalidBufferOptions,
    InvalidFilePath,
    DuplicateFilePath,
    MissingRootLogger,
    AlreadyFinished,
    AlreadyInitialized,
    NotInitialized,
    OpenFailed,
    OutOfMemory,
    SizeOverflow,
    LateLoggerRegistration,
};

struct LogConfigError {
    LogConfigErrorCode code = LogConfigErrorCode::None;
    std::string message;
    int system_error = 0;
};

template<typename T>
using LogConfigResult = std::expected<T, LogConfigError>;

struct FileAppenderOptions {
    std::string name;
    std::string path;
    mode_t file_mode = 0644;
    std::size_t buffer_size = 0;
    std::chrono::milliseconds flush_interval{0};
    LogLevel min_level = LogLevel::Trace;
    LogLevel max_level = LogLevel::Fatal;
};

struct ConsoleAppenderOptions {
    std::string name;
    ConsoleStream stream = ConsoleStream::Stderr;
    LogLevel min_level = LogLevel::Trace;
    LogLevel max_level = LogLevel::Fatal;
};

struct LoggerOptions {
    std::string name;
    std::optional<LogLevel> level;
    std::optional<unsigned> verbosity;
    bool additive = true;
};

struct RootLoggerOptions {
    LogLevel level = LogLevel::Info;
    unsigned verbosity = 0;
};

class LogConfig {
public:
    LogConfig() = default;
    LogConfig(const LogConfig &) = delete;
    LogConfig &operator=(const LogConfig &) = delete;
    LogConfig(LogConfig &&) noexcept = default;
    LogConfig &operator=(LogConfig &&) noexcept = default;

private:
    friend class LogConfigBuilder;
    friend class LoggerManager;

    struct AppenderDefinition {
        enum class Type : std::uint8_t {
            File,
            Console,
        };

        Type type = Type::File;
        FileAppenderOptions file;
        ConsoleAppenderOptions console;

        [[nodiscard]] std::string_view name() const noexcept {
            return type == Type::File ? std::string_view(file.name) : std::string_view(console.name);
        }
    };

    struct LoggerDefinition {
        LoggerOptions options;
        std::vector<AppenderId> appenders;
    };

    std::vector<AppenderDefinition> appenders_;
    std::vector<LoggerDefinition> loggers_;
    // Logger categories requested by runtime configuration rather than by static
    // LoggerHandle registration. LoggerManager materializes these at initialization.
    std::vector<std::string> requested_loggers_;
    RootLoggerOptions root_{};
    std::vector<AppenderId> root_appenders_;
    bool has_root_ = false;
};

class LogConfigBuilder {
public:
    [[nodiscard]] LogConfigResult<AppenderId> add_file_appender(FileAppenderOptions options);
    [[nodiscard]] LogConfigResult<AppenderId> add_console_appender(ConsoleAppenderOptions options);

    [[nodiscard]] LogConfigResult<void> add_logger(LoggerOptions options, std::initializer_list<AppenderId> appenders);
    [[nodiscard]] LogConfigResult<void> add_logger(LoggerOptions options, std::vector<AppenderId> appenders);
    [[nodiscard]] LogConfigResult<void> request_logger(std::string name);

    [[nodiscard]] LogConfigResult<void> set_root_logger(RootLoggerOptions options,
                                                        std::initializer_list<AppenderId> appenders);
    [[nodiscard]] LogConfigResult<void> set_root_logger(RootLoggerOptions options, std::vector<AppenderId> appenders);

    [[nodiscard]] LogConfigResult<LogConfig> finish();

private:
    [[nodiscard]] LogConfigResult<void> validate_appender_name(std::string_view name) const;
    [[nodiscard]] LogConfigResult<void> validate_appender_ids(const std::vector<AppenderId> &appenders) const;
    [[nodiscard]] LogConfigResult<void> ensure_building() const;

    LogConfig config_;
    bool finished_ = false;
};

[[nodiscard]] bool valid_logger_name(std::string_view name) noexcept;

} // namespace fiber::log

#endif // FIBER_LOG_LOG_CONFIG_H
