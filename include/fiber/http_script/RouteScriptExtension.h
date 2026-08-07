#ifndef FIBER_HTTP_SCRIPT_ROUTE_SCRIPT_EXTENSION_H
#define FIBER_HTTP_SCRIPT_ROUTE_SCRIPT_EXTENSION_H

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "../script/std/StdLibrary.h"
#include "ConstPackage.h"
#include "HttpClientFuncs.h"

namespace fiber::http_script {

// Compile-time extension that resolves configuration-dependent "$namespace.key" constants
// and HTTP directives through StdLibrary callbacks. A CompileScope supplies the current route
// metadata and the builder that owns constant callables for the enclosing immutable snapshot.
//
// Compile-time existence rules (the parser turns a nullptr from resolve_constant into a
// "constant not found" parse error):
//   $path.<name>   - <name> must be one of the location's route path variables, else
//                    compile fails. This is the headline compile-time check.
//   $query.<key>   - always resolvable (slot exists); value looked up at request time.
//   $header.<key>  - always resolvable; matched case-insensitively with '-' == '_'.
//   $cookie.<key>  - always resolvable; matched case-insensitively with '-' == '_'.
//
// Compilation copies each HostCallable into the script. Constant userdata is transferred from
// the builder into ConstPackage and is independent of this extension after compilation. HTTP
// directive definitions remain owned here, so directive-bearing scripts still require this
// extension to outlive them. The closed $req/$conn field sets are provided separately by
// ExchangeConstExtension and do not consume package indices.
class RouteScriptExtension {
public:
    using Library = fiber::script::Library;
    using DirectiveDef = Library::DirectiveDef;
    using HostCallable = Library::HostCallable;
    using HostCallFrame = Library::HostCallFrame;

    class CompileScope {
    public:
        CompileScope(RouteScriptExtension &extension, ConstPackage::Builder &builder,
                     std::span<const std::string> path_var_names, bool http_directives_enabled);
        ~CompileScope();

        CompileScope(const CompileScope &) = delete;
        CompileScope &operator=(const CompileScope &) = delete;
        CompileScope(CompileScope &&) = delete;
        CompileScope &operator=(CompileScope &&) = delete;

    private:
        RouteScriptExtension *extension_ = nullptr;
    };

    RouteScriptExtension() = default;

    [[nodiscard]] static const fiber::script::std_lib::StdLibrary::ExtOps &ops() noexcept;

private:
    static const HostCallable *resolve_constant_op(void *ctx, std::string_view namespace_name, std::string_view key);
    static DirectiveDef *resolve_directive_def_op(void *ctx, std::string_view type, std::string_view name,
                                                  const std::vector<fiber::script::JsValue> &literals);

    const HostCallable *resolve_constant(std::string_view namespace_name, std::string_view key);
    DirectiveDef *resolve_directive_def(std::string_view type, std::string_view name,
                                        const std::vector<fiber::script::JsValue> &literals);
    void begin_compile(ConstPackage::Builder &builder, std::span<const std::string> path_var_names,
                       bool http_directives_enabled);
    void end_compile() noexcept;

    std::unordered_set<std::string> current_path_var_names_;
    ConstPackage::Builder *const_builder_ = nullptr;
    bool allow_http_directives_ = false;
    // Owns HttpDirectiveDef instances created by `directive <name> = http "<target>";` across
    // runtime compilation. Kept alive because compiled host-call userdata points at each def.
    std::vector<std::unique_ptr<HttpDirectiveDef>> directive_defs_;
};

} // namespace fiber::http_script

#endif // FIBER_HTTP_SCRIPT_ROUTE_SCRIPT_EXTENSION_H
