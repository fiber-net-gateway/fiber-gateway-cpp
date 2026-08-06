#ifndef FIBER_ACCESS_SERVER_ACCESS_SCRIPT_RUNTIME_H
#define FIBER_ACCESS_SERVER_ACCESS_SCRIPT_RUNTIME_H

#include "../execution/AccessRequestHandler.h"
#include "../routing/ProjectRouteSnapshot.h"

#include <span>
#include <string>
#include <string_view>

#include <http_script/RouteScriptExtension.h>
#include <script/std/StdLibrary.h>

namespace fiber::access_server {

// Process-lifetime owner for the side-effect-free local expression library and the stable
// extension userdata referenced by compiled route expressions. Invocation state lives in
// AccessRequestTelemetry's request-scoped ScriptExchangeCtx.
class AccessScriptRuntime {
public:
    AccessScriptRuntime();

    [[nodiscard]] ScriptCompilerAdapter compiler_adapter() noexcept;
    [[nodiscard]] AccessRequestScriptAdapter request_adapter() noexcept;

private:
    [[nodiscard]] static ScriptCompilerAdapter::Result
    compile_expression(void *context, std::string_view expression, std::span<const std::string> path_variable_names);
    [[nodiscard]] static bool evaluate_condition(void *context, http_script::ScriptExchangeCtx &script_context,
                                                 std::span<const PathVariable> path_variables,
                                                 std::string_view request_context_cluster,
                                                 const script::Script &program) noexcept;
    [[nodiscard]] static bool evaluate_template(void *context, http_script::ScriptExchangeCtx &script_context,
                                                std::span<const PathVariable> path_variables,
                                                std::string_view request_context_cluster, const script::Script &program,
                                                std::string_view expression, std::string &output,
                                                AccessError &error) noexcept;

    script::std_lib::StdLibrary library_;
    http_script::RouteScriptExtension route_extension_;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_SCRIPT_RUNTIME_H
