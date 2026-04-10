#ifndef FIBER_LITE_NGINX_RUNTIME_RUNTIME_BUILDER_H
#define FIBER_LITE_NGINX_RUNTIME_RUNTIME_BUILDER_H

#include <expected>

#include "../config/Config.h"
#include "RuntimeConfig.h"

namespace fiber::lite_nginx::runtime {

class RuntimeBuilder {
public:
    static std::expected<RuntimeConfig, RuntimeError> build(const config::MainConfig &config);
};

} // namespace fiber::lite_nginx::runtime

#endif // FIBER_LITE_NGINX_RUNTIME_RUNTIME_BUILDER_H
