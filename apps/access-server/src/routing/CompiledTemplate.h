#ifndef FIBER_ACCESS_SERVER_COMPILED_TEMPLATE_H
#define FIBER_ACCESS_SERVER_COMPILED_TEMPLATE_H

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include <fiber/script/Script.h>

namespace fiber::access_server {

struct CompiledTemplateExpression {
    std::string leading_literal;
    std::string source;
    script::Script program;
};

struct CompiledTemplate {
    std::vector<CompiledTemplateExpression> expressions;
    std::string trailing_literal;
    std::size_t literal_size = 0;

    [[nodiscard]] bool dynamic() const noexcept { return !expressions.empty(); }
};

enum class TemplateParseError : std::uint8_t {
    InvalidEscape,
    EmptyExpression,
    UnclosedExpression,
};

// Parses ploto-unified-access templates. Escaping is intentionally limited to
// the Java RouteExecutionBuilder contract: only \\, \$, \{ and \} are valid,
// and escapes are interpreted only outside ${...} expressions.
[[nodiscard]] std::expected<CompiledTemplate, TemplateParseError> parse_template(std::string_view source);

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_COMPILED_TEMPLATE_H
