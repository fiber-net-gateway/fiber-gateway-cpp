#ifndef FIBER_SCRIPT_STD_OBJECTS_FUNCS_H
#define FIBER_SCRIPT_STD_OBJECTS_FUNCS_H

namespace fiber::script::std_lib {

class StdLibrary;

// Registers Object.assign / Object.keys / Object.values / Object.deleteProperties
// on the given library. Called once from StdLibrary's constructor; see StdLibrary.cpp.
void register_objects_funcs(StdLibrary &lib);

} // namespace fiber::script::std_lib

#endif // FIBER_SCRIPT_STD_OBJECTS_FUNCS_H
