#include "LoggingBuilder.h"

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fiber::lite_nginx::logging {
namespace {

fiber::log::LogLevel to_log_level(config::LoggingLevel level) noexcept {
    switch (level) {
        case config::LoggingLevel::Trace:
            return fiber::log::LogLevel::Trace;
        case config::LoggingLevel::Debug:
            return fiber::log::LogLevel::Debug;
        case config::LoggingLevel::Info:
            return fiber::log::LogLevel::Info;
        case config::LoggingLevel::Warn:
            return fiber::log::LogLevel::Warn;
        case config::LoggingLevel::Error:
            return fiber::log::LogLevel::Error;
        case config::LoggingLevel::Fatal:
            return fiber::log::LogLevel::Fatal;
    }
    return fiber::log::LogLevel::Info;
}

runtime::RuntimeError make_error(const config::SourceLocation &location, const fiber::log::LogConfigError &error) {
    std::string message = "logging configuration error: ";
    message.append(error.message);
    return runtime::RuntimeError{
            .message = std::move(message),
            .location = location,
    };
}

std::expected<fiber::log::LogConfig, runtime::RuntimeError> build_default() {
    fiber::log::LogConfigBuilder builder;
    auto stderr_id = builder.add_console_appender({
            .name = "default_stderr",
            .stream = fiber::log::ConsoleStream::Stderr,
    });
    if (!stderr_id) {
        return std::unexpected(make_error({}, stderr_id.error()));
    }
    auto root = builder.set_root_logger({.level = fiber::log::LogLevel::Info}, {*stderr_id});
    if (!root) {
        return std::unexpected(make_error({}, root.error()));
    }
    auto result = builder.finish();
    if (!result) {
        return std::unexpected(make_error({}, result.error()));
    }
    return std::move(*result);
}

} // namespace

std::expected<fiber::log::LogConfig, runtime::RuntimeError> LoggingBuilder::build(const config::LoggingConfig &config) {
    if (!config.configured) {
        return build_default();
    }

    fiber::log::LogConfigBuilder builder;
    std::unordered_map<std::string, fiber::log::AppenderId> appenders;
    appenders.reserve(config.appenders.size() * 2 + 1);

    for (const auto &appender: config.appenders) {
        fiber::log::LogConfigResult<fiber::log::AppenderId> added = std::unexpected(fiber::log::LogConfigError{});
        if (appender.kind == config::LogAppenderKind::File) {
            added = builder.add_file_appender({
                    .name = appender.name,
                    .path = appender.path,
                    .file_mode = static_cast<mode_t>(appender.file_mode),
                    .buffer_size = appender.buffer_size,
                    .flush_interval = appender.flush_interval,
                    .min_level = to_log_level(appender.min_level),
                    .max_level = to_log_level(appender.max_level),
            });
        } else {
            added = builder.add_console_appender({
                    .name = appender.name,
                    .stream = appender.stream == config::LogConsoleStream::Stdout ? fiber::log::ConsoleStream::Stdout
                                                                                  : fiber::log::ConsoleStream::Stderr,
                    .min_level = to_log_level(appender.min_level),
                    .max_level = to_log_level(appender.max_level),
            });
        }
        if (!added) {
            return std::unexpected(make_error(appender.location, added.error()));
        }
        appenders.emplace(appender.name, *added);
    }

    for (const auto &rule: config.loggers) {
        std::vector<fiber::log::AppenderId> targets;
        targets.reserve(rule.appenders.size());
        for (const auto &name: rule.appenders) {
            auto it = appenders.find(name);
            if (it == appenders.end()) {
                return std::unexpected(runtime::RuntimeError{
                        .message = "logger references unknown appender: " + name,
                        .location = rule.location,
                });
            }
            targets.push_back(it->second);
        }

        fiber::log::LoggerOptions options{
                .name = rule.name,
                .additive = rule.additive,
        };
        if (rule.level) {
            options.level = to_log_level(*rule.level);
        }
        options.verbosity = rule.verbosity;
        auto added = builder.add_logger(std::move(options), std::move(targets));
        if (!added) {
            return std::unexpected(make_error(rule.location, added.error()));
        }
    }

    std::vector<fiber::log::AppenderId> root_targets;
    root_targets.reserve(config.root.appenders.size());
    for (const auto &name: config.root.appenders) {
        auto it = appenders.find(name);
        if (it == appenders.end()) {
            return std::unexpected(runtime::RuntimeError{
                    .message = "root_logger references unknown appender: " + name,
                    .location = config.root.location,
            });
        }
        root_targets.push_back(it->second);
    }
    auto root = builder.set_root_logger(
            {
                    .level = to_log_level(config.root.level),
                    .verbosity = config.root.verbosity,
            },
            std::move(root_targets));
    if (!root) {
        return std::unexpected(make_error(config.root.location, root.error()));
    }

    auto result = builder.finish();
    if (!result) {
        return std::unexpected(make_error(config.location, result.error()));
    }
    return std::move(*result);
}

} // namespace fiber::lite_nginx::logging
