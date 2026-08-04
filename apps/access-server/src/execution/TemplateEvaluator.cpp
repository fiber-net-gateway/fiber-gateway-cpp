#include "TemplateEvaluator.h"

#include <utility>

namespace fiber::access_server {

std::expected<std::string, AccessError> evaluate_template(const CompiledTemplate &value, TemplateEvaluator evaluator) {
    std::string output;
    output.reserve(value.literal_size);

    for (const CompiledTemplateExpression &expression: value.expressions) {
        output.append(expression.leading_literal);
        if (!evaluator.evaluate) {
            return std::unexpected(AccessError::template_script("template evaluator is not configured"));
        }

        std::string expression_output;
        AccessError error = AccessError::template_script("template evaluation failed");
        if (!evaluator.evaluate(evaluator.context, expression.program.get(), expression.source, expression_output,
                                error)) {
            return std::unexpected(std::move(error));
        }
        output.append(expression_output);
    }
    output.append(value.trailing_literal);
    return output;
}

} // namespace fiber::access_server
