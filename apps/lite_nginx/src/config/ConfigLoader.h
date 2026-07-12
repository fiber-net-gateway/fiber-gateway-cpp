#ifndef FIBER_LITE_NGINX_CONFIG_CONFIG_LOADER_H
#define FIBER_LITE_NGINX_CONFIG_CONFIG_LOADER_H

#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "Ast.h"
#include "Config.h"

namespace fiber::lite_nginx::config {

class ConfigLoader {
public:
    static std::expected<Document, ConfigError> parse_document(std::string_view input, std::string source_name);
    static std::expected<MainConfig, ConfigError> load_from_string(std::string_view input, std::string source_name);
    static std::expected<MainConfig, ConfigError> load_from_file(const std::string &path);

private:
    // Recursively splices included files' top-level directives in place of each
    // `include` node, recursing into block children. Runs on the Document before
    // SemanticAnalyzer. `ancestors` is the stack of canonical paths currently being
    // expanded (for cycle detection). Mutates `directives` in place.
    static std::expected<void, ConfigError> expand_includes(std::vector<DirectiveNode> &directives,
                                                            std::vector<std::string> ancestors);

    // Reads and parses an included file at `canonical_path`, returning its Document
    // (whose directives carry source_name = canonical_path, so nested includes and
    // path directives resolve relative to the included file).
    static std::expected<Document, ConfigError> load_include_document(const std::string &canonical_path);
};

} // namespace fiber::lite_nginx::config

#endif // FIBER_LITE_NGINX_CONFIG_CONFIG_LOADER_H
