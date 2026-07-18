#ifndef FIBER_LITE_NGINX_LOGGING_LOGGING_BUILDER_H
#define FIBER_LITE_NGINX_LOGGING_LOGGING_BUILDER_H

#include <expected>

#include "log/LogConfig.h"

#include "../config/Config.h"
#include "../runtime/RuntimeConfig.h"

namespace fiber::lite_nginx::logging {

class LoggingBuilder {
public:
    [[nodiscard]] static std::expected<fiber::log::LogConfig, runtime::RuntimeError>
    build(const config::LoggingConfig &config);
};

} // namespace fiber::lite_nginx::logging

#endif // FIBER_LITE_NGINX_LOGGING_LOGGING_BUILDER_H
