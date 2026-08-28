#ifndef FIBER_HTTP_SCRIPT_RESPONSE_FUNCS_H
#define FIBER_HTTP_SCRIPT_RESPONSE_FUNCS_H

namespace fiber::script::std_lib {
class StdLibrary;
}

namespace fiber::http_script {

// Registers resp.* host functions (setHeader/addHeader/addCookie, plus async
// send/sendJson) on the given StdLibrary. The functions recover the per-request
// ScriptExchangeCtx from the host-call frame's attach pointer.
void register_response_funcs(fiber::script::std_lib::StdLibrary &lib);

} // namespace fiber::http_script

#endif // FIBER_HTTP_SCRIPT_RESPONSE_FUNCS_H
