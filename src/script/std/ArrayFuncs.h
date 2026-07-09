#ifndef FIBER_SCRIPT_STD_ARRAY_FUNCS_H
#define FIBER_SCRIPT_STD_ARRAY_FUNCS_H

namespace fiber::script::std_lib {

class StdLibrary;

// Registers array.join / array.pop / array.push on the given library.
// Called once from StdLibrary's constructor; see StdLibrary.cpp.
void register_array_funcs(StdLibrary &lib);

} // namespace fiber::script::std_lib

#endif // FIBER_SCRIPT_STD_ARRAY_FUNCS_H
