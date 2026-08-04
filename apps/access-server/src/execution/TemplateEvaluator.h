#ifndef FIBER_ACCESS_SERVER_TEMPLATE_EVALUATOR_H
#define FIBER_ACCESS_SERVER_TEMPLATE_EVALUATOR_H

#include "AccessError.h"

#include <expected>
#include <string>
#include <string_view>

#include "../routing/CompiledTemplate.h"

namespace fiber::access_server {

struct TemplateEvaluator {
    // On success, writes the Java JsonNode.asText("") compatible expression
    // result to `output`. The callback is an adapter boundary for this
    // repository's script engine and is not a Java VM compatibility promise.
    using Function = bool (*)(void *context, const void *program, std::string_view expression, std::string &output,
                              AccessError &error) noexcept;

    void *context = nullptr;
    Function evaluate = nullptr;
};

[[nodiscard]] std::expected<std::string, AccessError> evaluate_template(const CompiledTemplate &value,
                                                                        TemplateEvaluator evaluator);

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_TEMPLATE_EVALUATOR_H
