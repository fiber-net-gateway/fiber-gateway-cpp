#ifndef FIBER_LITE_NGINX_CONFIG_CONFIG_LOADER_H
#define FIBER_LITE_NGINX_CONFIG_CONFIG_LOADER_H

#include <expected>
#include <string>
#include <string_view>

#include "Ast.h"
#include "Config.h"

namespace fiber::lite_nginx::config {

class ConfigLoader {
public:
    static std::expected<Document, ConfigError> parse_document(std::string_view input, std::string source_name);
    static std::expected<MainConfig, ConfigError> load_from_string(std::string_view input, std::string source_name);
    static std::expected<MainConfig, ConfigError> load_from_file(const std::string &path);
};

} // namespace fiber::lite_nginx::config

#endif // FIBER_LITE_NGINX_CONFIG_CONFIG_LOADER_H
