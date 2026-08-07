#ifndef FIBER_SCRIPT_STD_JSON_FUNCS_H
#define FIBER_SCRIPT_STD_JSON_FUNCS_H

namespace fiber::script::std_lib {

class StdLibrary;

// Registers the JSON.* library functions on the given library:
//   JSON.parse(text)    parses a JSON document string into a value (Object/Array/
//                       scalar). Non-String input raises a catchable TypeError;
//                       a malformed document raises a catchable SyntaxError
//                       carrying the decoder's message and byte offset.
//   JSON.stringify(v)   serializes a value to a JSON text string. Top-level
//                       undefined yields undefined; top-level NaN/+/-Inf yields
//                       "null"; any other encode failure raises a catchable
//                       TypeError.
// Called once from StdLibrary's constructor; see StdLibrary.cpp.
void register_json_funcs(StdLibrary &lib);

} // namespace fiber::script::std_lib

#endif // FIBER_SCRIPT_STD_JSON_FUNCS_H
