#include "TemplateEvaluator.h"

#include <utility>

namespace fiber::access_server {

Result<std::string> evaluate_template(const CompiledTemplate &value, TemplateEvaluator evaluator) {
    std::string output;
    output.reserve(value.literal_size);

    for (const CompiledTemplateExpression &expression: value.expressions) {
        output.append(expression.leading_literal);
        if (!evaluator.evaluate) {
            return std::unexpected(Err::from_exception(Exception{
                    .name = "TEMPLATE_SCRIPT",
                    .message = "error exec for template expression: template evaluator is not configured",
                    .status = 500,
            }));
        }

        std::string expression_output;
        auto evaluated =
                evaluator.evaluate(evaluator.context, expression.program, expression.source, expression_output);
        if (!evaluated) {
            return std::unexpected(evaluated.error());
        }
        output.append(expression_output);
    }
    output.append(value.trailing_literal);
    return output;
}

} // namespace fiber::access_server
