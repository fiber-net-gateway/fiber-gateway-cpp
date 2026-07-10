#ifndef FIBER_SCRIPT_STD_BINARY_FUNCS_H
#define FIBER_SCRIPT_STD_BINARY_FUNCS_H

namespace fiber::script::std_lib {

class StdLibrary;

// Registers binary.base64Encode / binary.base64Decode / binary.hex /
// binary.fromHex / binary.getUtf8Bytes on the given library.
// Called once from StdLibrary's constructor; see StdLibrary.cpp.
void register_binary_funcs(StdLibrary &lib);

} // namespace fiber::script::std_lib

#endif // FIBER_SCRIPT_STD_BINARY_FUNCS_H
