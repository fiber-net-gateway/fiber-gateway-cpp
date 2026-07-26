#ifndef FIBER_AI_SERVER_AI_SERVER_LOGGING_H
#define FIBER_AI_SERVER_AI_SERVER_LOGGING_H

#include <log/LogConfig.h>

namespace fiber::ai_server {

[[nodiscard]] log::LogConfigResult<log::LogConfig> make_log_config();

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_AI_SERVER_LOGGING_H
