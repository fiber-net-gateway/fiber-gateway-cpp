#ifndef FIBER_HTTP_SCRIPT_REQUEST_FUNCS_H
#define FIBER_HTTP_SCRIPT_REQUEST_FUNCS_H

namespace fiber::script::std_lib {
class StdLibrary;
}

namespace fiber::http_script {

// Registers req.* host functions (getHeader/getQuery/getUri/getPath/getQueryStr/
// getMethod/getCookie, plus async readJson/readBinary/discardBody) on the given
// StdLibrary. The functions recover the per-request ScriptExchangeCtx from the host-call
// frame's attach pointer. Overloads (e.g. getHeader with/without a name argument) are
// registered as separate, non-overlapping-arity entries under the same name.
void register_request_funcs(fiber::script::std_lib::StdLibrary &lib);

} // namespace fiber::http_script

#endif // FIBER_HTTP_SCRIPT_REQUEST_FUNCS_H
