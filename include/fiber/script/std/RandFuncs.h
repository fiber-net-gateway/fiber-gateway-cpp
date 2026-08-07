#ifndef FIBER_SCRIPT_STD_RAND_FUNCS_H
#define FIBER_SCRIPT_STD_RAND_FUNCS_H

namespace fiber::script::std_lib {

class StdLibrary;

// Registers rand.random / rand.canary on the given library.
// Called once from StdLibrary's constructor; see StdLibrary.cpp.
void register_rand_funcs(StdLibrary &lib);

} // namespace fiber::script::std_lib

#endif // FIBER_SCRIPT_STD_RAND_FUNCS_H
