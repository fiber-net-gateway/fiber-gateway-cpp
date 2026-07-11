#ifndef FIBER_HTTP_SCRIPT_LIB_H
#define FIBER_HTTP_SCRIPT_LIB_H

namespace fiber::script::std_lib {
class StdLibrary;
}

namespace fiber::http_script {

// Registers all req.* and resp.* host functions (see RequestFuncs/ResponseFuncs) on the
// given StdLibrary. A host (e.g. lite_nginx) calls this once on its own StdLibrary
// instance after constructing it, so the script gains HTTP request/response functions
// alongside the standard library without polluting the process-wide singleton.
void register_http_functions_to_lib(fiber::script::std_lib::StdLibrary &lib);

} // namespace fiber::http_script

#endif // FIBER_HTTP_SCRIPT_LIB_H
