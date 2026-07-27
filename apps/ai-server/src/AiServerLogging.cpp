#include "AiServerLogging.h"

#include <utility>

namespace fiber::ai_server {

log::LogConfigResult<AiServerLogConfig> make_log_config(const LlmAuditLogOptions &audit) {
    log::LogConfigBuilder builder;
    auto stderr_appender = builder.add_console_appender({
            .name = "ai_server_stderr",
            .stream = log::ConsoleStream::Stderr,
    });
    if (!stderr_appender) {
        return std::unexpected(std::move(stderr_appender.error()));
    }

    log::FileAppenderOptions audit_file{
            .name = "ai_server_audit_file",
            .path = audit.path,
            .file_mode = 0600,
            .min_level = log::LogLevel::Info,
            .max_level = log::LogLevel::Info,
            .no_follow = true,
            .regular_file_only = true,
            .enforce_file_mode = true,
            .truncate_incomplete_tail = true,
    };
    if (audit.rotate_bytes != 0) {
        audit_file.rotation = log::FileRotationOptions{
                .max_file_size = audit.rotate_bytes,
                .archive_name = "{base}.{seq}",
                .max_archives = audit.max_archives,
        };
    }
    auto audit_appender = builder.add_file_appender(std::move(audit_file));
    if (!audit_appender) {
        return std::unexpected(std::move(audit_appender.error()));
    }

    auto audit_logger = builder.add_logger(
            {
                    .name = "ai_server.audit",
                    .level = log::LogLevel::Info,
                    .additive = false,
            },
            {*audit_appender});
    if (!audit_logger) {
        return std::unexpected(std::move(audit_logger.error()));
    }

    auto async = builder.set_async_options({
            .backlog_capacity = log::kDefaultLogBacklogCapacity,
            .full_policy = log::LogQueueFullPolicy::DropNewest,
    });
    if (!async) {
        return std::unexpected(std::move(async.error()));
    }

    auto root = builder.set_root_logger(
            {
                    .level = log::LogLevel::Debug,
            },
            {*stderr_appender});
    if (!root) {
        return std::unexpected(std::move(root.error()));
    }
    auto config = builder.finish();
    if (!config) {
        return std::unexpected(std::move(config.error()));
    }
    return AiServerLogConfig{
            .config = std::move(*config),
            .audit_appender_id = *audit_appender,
    };
}

} // namespace fiber::ai_server
