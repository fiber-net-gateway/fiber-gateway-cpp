#ifndef FIBER_SCRIPT_STD_STRINGS_FUNCS_H
#define FIBER_SCRIPT_STD_STRINGS_FUNCS_H

namespace fiber::script::std_lib {

class StdLibrary;

// Registers the strings.* library functions on the given library:
//   strings.hasPrefix / hasSuffix / toLower / toUpper / trim / trimLeft /
//   trimRight / split / contains / contains_any / index / indexAny / lastIndex /
//   lastIndexAny / repeat / substring / toString
// Called once from StdLibrary's constructor; see StdLibrary.cpp.
//
// strings.match and strings.findAll (regex) are intentionally NOT registered
// here yet: the regex engine choice is deferred. Add them alongside a regex
// implementation when that decision is made.
void register_strings_funcs(StdLibrary &lib);

} // namespace fiber::script::std_lib

#endif // FIBER_SCRIPT_STD_STRINGS_FUNCS_H
