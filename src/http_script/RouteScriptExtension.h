#ifndef FIBER_HTTP_SCRIPT_ROUTE_SCRIPT_EXTENSION_H
#define FIBER_HTTP_SCRIPT_ROUTE_SCRIPT_EXTENSION_H

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../script/std/StdLibrary.h"
#include "HttpClientFuncs.h"

namespace fiber::http_script {

// Runtime-owned extension that resolves route-scoped "$namespace.key" constants and HTTP
// directives through StdLibrary's extension callbacks. RuntimeBuilder updates the current route
// information before each serial script compilation.
//
// Compile-time existence rules (the parser turns a nullptr from resolve_constant into a
// "constant not found" parse error):
//   $path.<name>   - <name> must be one of the location's route path variables, else
//                    compile fails. This is the headline compile-time check.
//   $req.<field>   - <field> must be one of {uri, method, path, query}, else compile fails.
//   $query.<key>   - always resolvable (slot exists); value looked up at request time.
//   $header.<key>  - always resolvable; matched case-insensitively with '-' == '_'.
//   $cookie.<key>  - always resolvable; matched case-insensitively with '-' == '_'.
//   $conn.<field>  - <field> must be one of {remote_addr, remote_port, http_version,
//                    scheme, tls}, else compile fails.
//
// Compilation copies each HostCallable's function pointer and userdata into the script. The
// userdata points at state owned here, so this object must outlive every script compiled with it.
class RouteScriptExtension {
public:
    using Library = fiber::script::Library;
    using DirectiveDef = Library::DirectiveDef;
    using HostCallable = Library::HostCallable;
    using HostCallFrame = Library::HostCallFrame;

    RouteScriptExtension() = default;

    [[nodiscard]] static const fiber::script::std_lib::StdLibrary::ExtOps &ops() noexcept;

    // RuntimeBuilder calls these before each serial compile. Route information is used only for
    // compile-time validation; cached HostCallables never point at this mutable route state.
    void set_compile_path_vars(const std::vector<std::string> &path_var_names);
    void set_http_directives_enabled(bool enabled) noexcept { allow_http_directives_ = enabled; }

private:
    enum class VarKind : std::uint8_t { Path, Query, Header, Cookie, ReqField, ConnField };

    struct VarRef {
        VarKind kind;
        std::string name; // normalized: lowercase for header/cookie; exact for path/query/req/conn
    };

    // Constant implementations (static so their addresses match Library::Constant). userdata
    // points at a VarRef owned by *this.
    static fiber::script::AbiResult path_var_fn(void *userdata, const HostCallFrame &frame) noexcept;
    static fiber::script::AbiResult query_var_fn(void *userdata, const HostCallFrame &frame) noexcept;
    static fiber::script::AbiResult header_var_fn(void *userdata, const HostCallFrame &frame) noexcept;
    static fiber::script::AbiResult cookie_var_fn(void *userdata, const HostCallFrame &frame) noexcept;
    static fiber::script::AbiResult req_field_fn(void *userdata, const HostCallFrame &frame) noexcept;
    static fiber::script::AbiResult conn_field_fn(void *userdata, const HostCallFrame &frame) noexcept;

    static const HostCallable *resolve_constant_op(void *ctx, std::string_view namespace_name, std::string_view key);
    static DirectiveDef *resolve_directive_def_op(void *ctx, std::string_view type, std::string_view name,
                                                  const std::vector<fiber::script::JsValue> &literals);

    const HostCallable *resolve_constant(std::string_view namespace_name, std::string_view key);
    DirectiveDef *resolve_directive_def(std::string_view type, std::string_view name,
                                        const std::vector<fiber::script::JsValue> &literals);
    const HostCallable *get_or_create(VarKind kind, std::string_view namespace_name, std::string_view key);

    std::unordered_set<std::string> current_path_var_names_;
    bool has_current_route_info_ = false;
    bool allow_http_directives_ = false;
    // Owns HttpDirectiveDef instances created by `directive <name> = http "<target>";` across
    // runtime compilation. Kept alive because compiled host-call userdata points at each def.
    std::vector<std::unique_ptr<HttpDirectiveDef>> directive_defs_;

    // Stable storage: deque elements keep their address across push_back; unordered_map nodes
    // keep their mapped value address across other inserts. Both are populated during compile.
    std::deque<VarRef> refs_;
    std::unordered_map<std::string, HostCallable> cache_;
};

} // namespace fiber::http_script

#endif // FIBER_HTTP_SCRIPT_ROUTE_SCRIPT_EXTENSION_H
