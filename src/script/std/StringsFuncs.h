#ifndef FIBER_SCRIPT_STD_STRINGS_FUNCS_H
#define FIBER_SCRIPT_STD_STRINGS_FUNCS_H

namespace fiber::script::std_lib {

class StdLibrary;

// Registers the strings.* library functions on the given library:
//   strings.hasPrefix / hasSuffix / toLower / toUpper / trim / trimLeft /
//   trimRight / split / match / findAll / contains / contains_any / index /
//   indexAny / lastIndex / lastIndexAny / repeat / substring / toString
// Called once from StdLibrary's constructor; see StdLibrary.cpp.
void register_strings_funcs(StdLibrary &lib);

} // namespace fiber::script::std_lib

#endif // FIBER_SCRIPT_STD_STRINGS_FUNCS_H
