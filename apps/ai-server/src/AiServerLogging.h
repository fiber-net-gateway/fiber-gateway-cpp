#ifndef FIBER_AI_SERVER_AI_SERVER_LOGGING_H
#define FIBER_AI_SERVER_AI_SERVER_LOGGING_H

#include "audit/LlmAuditLog.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include <log/LogConfig.h>

namespace fiber::ai_server {

enum class AiServerLoggingErrorCode : std::uint8_t {
    OpenFailed,
    ReadFailed,
    FileTooLarge,
    InvalidJson,
    UnsupportedVersion,
    MissingField,
    UnknownField,
    DuplicateField,
    InvalidValue,
    InvalidReference,
    ReservedName,
    BuildFailed,
};

struct AiServerLoggingError {
    AiServerLoggingErrorCode code = AiServerLoggingErrorCode::InvalidJson;
    std::size_t line = 0;
    std::size_t column = 0;
    std::size_t offset = 0;
    std::string field;
    std::string detail;
    int system_error = 0;
};

struct AiServerLogConfig {
    log::LogConfig config;
    LlmAuditLogOptions audit;
    log::AppenderId audit_appender_id = log::kInvalidAppenderId;
};

[[nodiscard]] std::expected<AiServerLogConfig, AiServerLoggingError> load_ai_server_log_config(std::string_view path);

[[nodiscard]] std::expected<AiServerLogConfig, AiServerLoggingError>
parse_ai_server_log_config(std::string_view content, std::string_view source_path);

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_AI_SERVER_LOGGING_H
