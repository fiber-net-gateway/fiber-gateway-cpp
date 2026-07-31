#include "AccessScriptRuntime.h"

#include <memory>
#include <utility>

#include <http_script/HttpScriptLib.h>
#include <http_script/ScriptExchangeCtx.h>
#include <script/JsGc.h>
#include <script/JsValue.h>
#include <script/Script.h>
#include <script/ScriptCompiler.h>
#include <script/run/Compares.h>
#include <script/std/NodeText.h>

namespace fiber::access_server {
namespace {

struct InvocationVariables {
    std::span<const PathVariable> path_variables;
    std::string_view context_cluster;
};

bool lookup_path_variable(void *context, std::string_view name, std::string_view &value) noexcept {
    const auto &variables = *static_cast<const InvocationVariables *>(context);
    for (const PathVariable &entry: variables.path_variables) {
        if (entry.name == name) {
            value = entry.value;
            return true;
        }
    }
    return false;
}

bool is_context_cluster_key(std::string_view name) noexcept {
    if (name == "cluster") {
        return true;
    }
    constexpr std::string_view kNormalizedKey = "hi_trace_cluster";
    if (name.size() != kNormalizedKey.size()) {
        return false;
    }
    for (std::size_t i = 0; i < name.size(); ++i) {
        unsigned char ch = static_cast<unsigned char>(name[i]);
        if (ch == '-') {
            ch = '_';
        } else if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<unsigned char>(ch - 'A' + 'a');
        }
        if (ch != static_cast<unsigned char>(kNormalizedKey[i])) {
            return false;
        }
    }
    return true;
}

bool lookup_context_variable(void *context, std::string_view name, std::string_view &value) noexcept {
    const auto &variables = *static_cast<const InvocationVariables *>(context);
    if (!is_context_cluster_key(name) || variables.context_cluster.empty()) {
        return false;
    }
    value = variables.context_cluster;
    return true;
}

std::string script_failure_message(const script::ScriptResult &result) {
    if (result.is_abort()) {
        std::string message = "local script aborted: ";
        message.append(script::abort_reason_name(result.abort().reason));
        return message;
    }
    if (result.is_exception()) {
        return "local script raised an exception";
    }
    return "local script returned no value";
}

} // namespace

AccessScriptRuntime::AccessScriptRuntime() {
    http_script::register_http_functions_to_lib(library_);
    library_.add_ext_ops(&route_extension_, http_script::RouteScriptExtension::ops());
}

ScriptCompilerAdapter AccessScriptRuntime::compiler_adapter() noexcept {
    return ScriptCompilerAdapter{
            .context = this,
            .compile_expression = compile_expression,
    };
}

AccessRequestScriptAdapter AccessScriptRuntime::request_adapter() noexcept {
    return AccessRequestScriptAdapter{
            .context = this,
            .evaluate_condition = evaluate_condition,
            .evaluate_template = evaluate_template,
    };
}

ScriptCompilerAdapter::Result
AccessScriptRuntime::compile_expression(void *context, std::string_view expression,
                                        std::span<const std::string> path_variable_names) {
    auto &runtime = *static_cast<AccessScriptRuntime *>(context);
    runtime.route_extension_.set_compile_path_vars(path_variable_names);
    runtime.route_extension_.set_http_directives_enabled(false);

    std::string source;
    source.reserve(expression.size() + 8);
    source.append("return ");
    source.append(expression);
    source.push_back(';');
    auto compiled = script::compile_script(runtime.library_, source, false);
    if (!compiled) {
        std::string message = compiled.error().message;
        message.append(" at expression offset ");
        message.append(std::to_string(compiled.error().position));
        return std::unexpected(std::move(message));
    }
    if (compiled->contains_async()) {
        return std::unexpected("asynchronous route expressions are not supported");
    }
    return std::static_pointer_cast<const void>(std::make_shared<const script::Script>(std::move(*compiled)));
}

bool AccessScriptRuntime::evaluate_condition(void *, http::HttpExchange &exchange,
                                             std::span<const PathVariable> path_variables,
                                             std::string_view request_context_cluster, const void *program,
                                             std::string_view) noexcept {
    if (!program) {
        return false;
    }
    const auto &script_program = *static_cast<const script::Script *>(program);
    script::GcHeap heap(exchange.pool());
    http_script::ScriptExchangeCtx script_context(exchange, heap);
    InvocationVariables variables{
            .path_variables = path_variables,
            .context_cluster = request_context_cluster,
    };
    script_context.set_path_var_lookup(&variables, lookup_path_variable);
    script_context.set_context_var_lookup(&variables, lookup_context_variable);
    auto result = script_program.exec_sync(script::JsValue::make_null(), &script_context, heap);
    if (!result.is_value()) {
        return false;
    }
    script::JsValue value = result.value();
    return script::run::Compares::logic(script::ConstValueHandle(&value));
}

bool AccessScriptRuntime::evaluate_template(void *, http::HttpExchange &exchange,
                                            std::span<const PathVariable> path_variables,
                                            std::string_view request_context_cluster, const void *program,
                                            std::string_view, std::string &output, AccessError &error) noexcept {
    if (!program) {
        error = AccessError::template_script("compiled local script is missing");
        return false;
    }
    const auto &script_program = *static_cast<const script::Script *>(program);
    script::GcHeap heap(exchange.pool());
    http_script::ScriptExchangeCtx script_context(exchange, heap);
    InvocationVariables variables{
            .path_variables = path_variables,
            .context_cluster = request_context_cluster,
    };
    script_context.set_path_var_lookup(&variables, lookup_path_variable);
    script_context.set_context_var_lookup(&variables, lookup_context_variable);
    auto result = script_program.exec_sync(script::JsValue::make_null(), &script_context, heap);
    if (!result.is_value()) {
        error = AccessError::template_script(script_failure_message(result));
        return false;
    }
    const script::JsNodeType type = script::js_value_type(result.value());
    if (type != script::JsNodeType::Null && type != script::JsNodeType::Undefined) {
        script::std_lib::node_as_text(result.value(), output);
    }
    return true;
}

} // namespace fiber::access_server
