#ifndef FIBER_HTTP_SCRIPT_ROUTE_SCRIPT_LIBRARY_H
#define FIBER_HTTP_SCRIPT_ROUTE_SCRIPT_LIBRARY_H

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../script/Library.h"
#include "HttpClientFuncs.h"

namespace fiber::script::std_lib {
class StdLibrary;
}

namespace fiber::http_script {

// Per-location Library that layers route-scoped "$namespace.key" constants on top of a
// shared StdLibrary. Function/directive resolution is delegated to the shared library so
// req.*/resp.* and standard functions resolve unchanged; only resolve_constant is
// overridden to synthesize HostCallables for the route-variable namespaces.
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
// The synthesized HostCallable pointers are baked into the compiled script, so this object
// must outlive the script (the host stores it in the location runtime, kept alive for the
// process lifetime alongside the shared library).
class RouteScriptLibrary : public fiber::script::Library {
public:
    RouteScriptLibrary(fiber::script::std_lib::StdLibrary &shared, const std::vector<std::string> &path_var_names);

    // ---- Delegated to the shared StdLibrary ----
    void mark_root_prop(std::string_view prop_name) override;
    FunctionMatchResult resolve_func(std::string_view name, const FunctionMatchRequest &request) const override;
    FunctionMatchResult resolve_async_func(std::string_view name, const FunctionMatchRequest &request) const override;
    DirectiveDef *resolve_directive_def(std::string_view type, std::string_view name,
                                        const std::vector<fiber::script::JsValue> &literals) const override;

    // ---- Route-variable constant synthesis ----
    const HostCallable *resolve_constant(std::string_view namespace_name, std::string_view key) const override;
    const HostCallable *resolve_async_constant(std::string_view namespace_name, std::string_view key) const override;

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

    const HostCallable *get_or_create(VarKind kind, std::string_view namespace_name, std::string_view key) const;

    fiber::script::std_lib::StdLibrary &shared_;
    std::unordered_set<std::string> path_var_names_;
    // Owns HttpDirectiveDef instances created by `directive <name> = http "<target>";` in this
    // location's script. Kept alive for the script's lifetime (HostCallables are baked in).
    mutable std::vector<std::unique_ptr<HttpDirectiveDef>> directive_defs_;

    // Stable storage: deque elements keep their address across push_back; unordered_map
    // nodes keep their mapped value address across other inserts. Both are mutated lazily
    // during compile (resolve_constant is const), hence mutable.
    mutable std::deque<VarRef> refs_;
    mutable std::unordered_map<std::string, HostCallable> cache_;
};

} // namespace fiber::http_script

#endif // FIBER_HTTP_SCRIPT_ROUTE_SCRIPT_LIBRARY_H
