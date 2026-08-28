#ifndef FIBER_SCRIPT_STD_LENGTH_FUNC_H
#define FIBER_SCRIPT_STD_LENGTH_FUNC_H

namespace fiber::script::std_lib {

class StdLibrary;

// Registers length() on the given library.
// Called once from StdLibrary's constructor; see StdLibrary.cpp.
void register_length_func(StdLibrary &lib);

} // namespace fiber::script::std_lib

#endif // FIBER_SCRIPT_STD_LENGTH_FUNC_H
