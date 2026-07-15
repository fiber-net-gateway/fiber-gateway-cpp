#ifndef FIBER_HTTP_SCRIPT_HTTP_CLIENT_FUNCS_H
#define FIBER_HTTP_SCRIPT_HTTP_CLIENT_FUNCS_H

#include <string_view>

#include "../script/AsyncTask.h"
#include "../script/Library.h"
#include "../script/ScriptResult.h"

#include "HttpTarget.h"

namespace fiber::script::std_lib {
class StdLibrary;
}

namespace fiber::http_script {

// Async host functions backing svc.request / svc.proxyPass, invoked via a `directive <name> =
// http "<target>";` binding. userdata is the HttpDirectiveDef carrying the compile-time-bound
// upstream host target; the host never comes from the options object. options.url is the request
// path?query, not a host. proxyPass({websocket:true}) translates an inbound HTTP/1.1
// WebSocket Upgrade or HTTP/2/3 Extended CONNECT to an upstream HTTP/1.1 Upgrade and
// holds the async host call until the bidirectional tunnel closes.
fiber::script::AsyncTask http_request_fn(void *userdata, const fiber::script::Library::HostCallFrame &frame,
                                         fiber::script::Library::Arguments args) noexcept;
fiber::script::AsyncTask http_proxy_pass_fn(void *userdata, const fiber::script::Library::HostCallFrame &frame,
                                            fiber::script::Library::Arguments args) noexcept;

// Directive def for `directive <name> = http "<target>";`. Binds a named script handle to a fixed
// upstream/URL target; svc.request / svc.proxyPass then resolve to the bound target via userdata.
// Owned by RouteScriptLibrary (which keeps it alive for the compiled script's lifetime).
class HttpDirectiveDef : public fiber::script::Library::DirectiveDef {
public:
    explicit HttpDirectiveDef(HttpTargetSpec target) noexcept;

    [[nodiscard]] const HttpTargetSpec &target() const noexcept { return target_; }

    fiber::script::Library::FunctionMatchResult
    resolve_func(std::string_view directive, std::string_view function,
                 const fiber::script::Library::FunctionMatchRequest &request,
                 const fiber::script::Library &library) const override;
    fiber::script::Library::FunctionMatchResult
    resolve_async_func(std::string_view directive, std::string_view function,
                       const fiber::script::Library::FunctionMatchRequest &request,
                       const fiber::script::Library &library) const override;

private:
    HttpTargetSpec target_;
    fiber::script::Library::HostCallable request_callable_{};
    fiber::script::Library::HostCallable proxy_pass_callable_{};
};

} // namespace fiber::http_script

#endif // FIBER_HTTP_SCRIPT_HTTP_CLIENT_FUNCS_H
