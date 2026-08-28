#include <fiber/http_script/HttpScriptLib.h>

#include <fiber/http_script/RequestFuncs.h>
#include <fiber/script/std/StdLibrary.h>
#include "http_script/ResponseFuncs.h"

namespace fiber::http_script {

void register_http_functions_to_lib(fiber::script::std_lib::StdLibrary &lib) {
    register_request_funcs(lib);
    register_response_funcs(lib);
}

} // namespace fiber::http_script
