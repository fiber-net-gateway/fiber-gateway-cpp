#ifndef FIBER_ACCESS_SERVER_TEMPLATE_EVALUATOR_H
#define FIBER_ACCESS_SERVER_TEMPLATE_EVALUATOR_H

#include "AccessResult.h"

#include <string>
#include <string_view>

#include "../routing/CompiledTemplate.h"

namespace fiber::access_server {

struct TemplateEvaluator {
    // On success, writes the Java JsonNode.asText("") compatible expression
    // result to `output`. The callback is an adapter boundary for this
    // repository's script engine and is not a Java VM compatibility promise.
    using Function = Result<void> (*)(void *context, const script::Script &program, std::string_view expression,
                                      std::string &output) noexcept;

    void *context = nullptr;
    Function evaluate = nullptr;
};

[[nodiscard]] Result<std::string> evaluate_template(const CompiledTemplate &value, TemplateEvaluator evaluator);

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_TEMPLATE_EVALUATOR_H
