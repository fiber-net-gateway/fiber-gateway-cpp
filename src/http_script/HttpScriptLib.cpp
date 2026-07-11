#include "HttpScriptLib.h"

#include "../script/std/StdLibrary.h"
#include "HttpClientFuncs.h"
#include "RequestFuncs.h"
#include "ResponseFuncs.h"

namespace fiber::http_script {

void register_http_functions_to_lib(fiber::script::std_lib::StdLibrary &lib) {
    register_request_funcs(lib);
    register_response_funcs(lib);
    register_http_client_funcs(lib);
}

} // namespace fiber::http_script
