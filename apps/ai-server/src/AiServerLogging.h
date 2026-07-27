#ifndef FIBER_AI_SERVER_AI_SERVER_LOGGING_H
#define FIBER_AI_SERVER_AI_SERVER_LOGGING_H

#include "audit/LlmAuditLog.h"

#include <log/LogConfig.h>

namespace fiber::ai_server {

struct AiServerLogConfig {
    log::LogConfig config;
    log::AppenderId audit_appender_id = log::kInvalidAppenderId;
};

[[nodiscard]] log::LogConfigResult<AiServerLogConfig> make_log_config(const LlmAuditLogOptions &audit);

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_AI_SERVER_LOGGING_H
