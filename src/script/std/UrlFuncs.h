#ifndef FIBER_SCRIPT_STD_URL_FUNCS_H
#define FIBER_SCRIPT_STD_URL_FUNCS_H

namespace fiber::script::std_lib {

class StdLibrary;

// Registers URL.encodeComponent / URL.decodeComponent / URL.parseQuery / URL.buildQuery
// on the given library. Called once from StdLibrary's constructor; see StdLibrary.cpp.
// The form-urlencoded codec itself lives in common/url/UrlForm; this file is the thin
// JsValue <-> string adapter.
void register_url_funcs(StdLibrary &lib);

} // namespace fiber::script::std_lib

#endif // FIBER_SCRIPT_STD_URL_FUNCS_H
