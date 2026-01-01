#ifndef FIBER_SCRIPT_SCRIPT_COMPILER_H
#define FIBER_SCRIPT_SCRIPT_COMPILER_H

#include <expected>
#include <memory>
#include <string_view>

#include "Library.h"
#include "Script.h"
#include "parse/ParseError.h"

namespace fiber::script {

std::expected<Script, parse::ParseError> compile_script(Library &library,
                                                        std::string_view script,
                                                        bool allow_assign = true);

} // namespace fiber::script

#endif // FIBER_SCRIPT_SCRIPT_COMPILER_H
