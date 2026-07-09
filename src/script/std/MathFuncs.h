#ifndef FIBER_SCRIPT_STD_MATH_FUNCS_H
#define FIBER_SCRIPT_STD_MATH_FUNCS_H

namespace fiber::script::std_lib {

class StdLibrary;

// Registers math.floor / math.abs on the given library.
// Called once from StdLibrary's constructor; see StdLibrary.cpp.
void register_math_funcs(StdLibrary &lib);

} // namespace fiber::script::std_lib

#endif // FIBER_SCRIPT_STD_MATH_FUNCS_H
