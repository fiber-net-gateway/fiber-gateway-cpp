#ifndef FIBER_SCRIPT_STD_INCLUDES_FUNC_H
#define FIBER_SCRIPT_STD_INCLUDES_FUNC_H

namespace fiber::script::std_lib {

class StdLibrary;

// Registers includes() on the given library.
// Called once from StdLibrary's constructor; see StdLibrary.cpp.
void register_includes_func(StdLibrary &lib);

} // namespace fiber::script::std_lib

#endif // FIBER_SCRIPT_STD_INCLUDES_FUNC_H
