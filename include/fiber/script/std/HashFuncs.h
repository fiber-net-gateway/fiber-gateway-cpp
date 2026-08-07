#ifndef FIBER_SCRIPT_STD_HASH_FUNCS_H
#define FIBER_SCRIPT_STD_HASH_FUNCS_H

namespace fiber::script::std_lib {

class StdLibrary;

// Registers hash.crc32 / hash.md5 / hash.sha1 / hash.sha256 on the given library.
// Called once from StdLibrary's constructor; see StdLibrary.cpp.
void register_hash_funcs(StdLibrary &lib);

} // namespace fiber::script::std_lib

#endif // FIBER_SCRIPT_STD_HASH_FUNCS_H
