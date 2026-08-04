#include "CompiledTemplate.h"

#include <utility>

namespace fiber::access_server {

std::expected<CompiledTemplate, TemplateParseError> parse_template(std::string_view source) {
    CompiledTemplate result;
    std::string literal;
    std::string expression;
    literal.reserve(source.size());

    bool escaping = false;
    bool in_expression = false;
    for (std::size_t i = 0; i < source.size(); ++i) {
        const char ch = source[i];
        if (escaping) {
            if (ch != '\\' && ch != '$' && ch != '{' && ch != '}') {
                return std::unexpected(TemplateParseError::InvalidEscape);
            }
            literal.push_back(ch);
            escaping = false;
            continue;
        }
        if (in_expression) {
            if (ch != '}') {
                expression.push_back(ch);
                continue;
            }
            if (expression.empty()) {
                return std::unexpected(TemplateParseError::EmptyExpression);
            }
            result.literal_size += literal.size();
            result.expressions.push_back(CompiledTemplateExpression{
                    .leading_literal = std::move(literal),
                    .source = std::move(expression),
            });
            literal.clear();
            expression.clear();
            in_expression = false;
            continue;
        }
        if (ch == '\\') {
            escaping = true;
        } else if (ch == '$' && i + 1 < source.size() && source[i + 1] == '{') {
            ++i;
            in_expression = true;
        } else {
            literal.push_back(ch);
        }
    }

    if (escaping) {
        return std::unexpected(TemplateParseError::InvalidEscape);
    }
    if (in_expression) {
        return std::unexpected(TemplateParseError::UnclosedExpression);
    }
    result.literal_size += literal.size();
    result.trailing_literal = std::move(literal);
    return result;
}

} // namespace fiber::access_server
