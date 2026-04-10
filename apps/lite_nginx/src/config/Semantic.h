#ifndef FIBER_LITE_NGINX_CONFIG_SEMANTIC_H
#define FIBER_LITE_NGINX_CONFIG_SEMANTIC_H

#include <expected>

#include "Ast.h"
#include "Config.h"

namespace fiber::lite_nginx::config {

class SemanticAnalyzer {
public:
    [[nodiscard]] std::expected<MainConfig, ConfigError> analyze(const Document &document) const;
};

} // namespace fiber::lite_nginx::config

#endif // FIBER_LITE_NGINX_CONFIG_SEMANTIC_H
