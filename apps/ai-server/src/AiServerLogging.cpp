#include "AiServerLogging.h"

#include <utility>

namespace fiber::ai_server {

log::LogConfigResult<log::LogConfig> make_log_config() {
    log::LogConfigBuilder builder;
    auto stderr_appender = builder.add_console_appender({
            .name = "ai_server_stderr",
            .stream = log::ConsoleStream::Stderr,
    });
    if (!stderr_appender) {
        return std::unexpected(std::move(stderr_appender.error()));
    }

    auto root = builder.set_root_logger(
            {
                    .level = log::LogLevel::Info,
            },
            {*stderr_appender});
    if (!root) {
        return std::unexpected(std::move(root.error()));
    }
    return builder.finish();
}

} // namespace fiber::ai_server
