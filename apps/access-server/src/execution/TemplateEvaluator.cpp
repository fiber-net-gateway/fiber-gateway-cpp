#include "TemplateEvaluator.h"

#include <utility>

namespace fiber::access_server {

std::expected<std::string, AccessError> evaluate_template(std::string_view source, TemplateEvaluator evaluator) {
    return evaluate_template(source, {}, evaluator);
}

std::expected<std::string, AccessError> evaluate_template(std::string_view source,
                                                          std::span<const CompiledScriptProgram> expression_programs,
                                                          TemplateEvaluator evaluator) {
    std::string output;
    output.reserve(source.size());

    bool escaping = false;
    bool expression = false;
    std::string expression_source;
    std::size_t expression_index = 0;
    for (std::size_t i = 0; i < source.size(); ++i) {
        const char ch = source[i];
        if (escaping) {
            (expression ? expression_source : output).push_back(ch);
            escaping = false;
            continue;
        }
        if (ch == '\\') {
            escaping = true;
            continue;
        }
        if (expression) {
            if (ch != '}') {
                expression_source.push_back(ch);
                continue;
            }

            if (!evaluator.evaluate) {
                return std::unexpected(AccessError::template_script("template evaluator is not configured"));
            }
            const void *program = nullptr;
            if (!expression_programs.empty()) {
                if (expression_index >= expression_programs.size()) {
                    return std::unexpected(AccessError::template_script("invalid compiled template program"));
                }
                program = expression_programs[expression_index].get();
            }
            std::string value;
            AccessError error = AccessError::template_script("template evaluation failed");
            if (!evaluator.evaluate(evaluator.context, program, expression_source, value, error)) {
                return std::unexpected(std::move(error));
            }
            output.append(value);
            ++expression_index;
            expression_source.clear();
            expression = false;
            continue;
        }
        if (ch == '$' && i + 1 < source.size() && source[i + 1] == '{') {
            ++i;
            expression = true;
        } else {
            output.push_back(ch);
        }
    }

    if (escaping || expression) {
        return std::unexpected(AccessError::template_script("invalid compiled template"));
    }
    if (!expression_programs.empty() && expression_index != expression_programs.size()) {
        return std::unexpected(AccessError::template_script("invalid compiled template program"));
    }
    return output;
}

} // namespace fiber::access_server
