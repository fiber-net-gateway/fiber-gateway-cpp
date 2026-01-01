#include "ScriptCompiler.h"

#include <string>
#include <utility>

#include "ir/Compiler.h"
#include "parse/Optimiser.h"
#include "parse/Parser.h"

namespace fiber::script {

std::expected<Script, parse::ParseError> compile_script(Library &library,
                                                        std::string_view script,
                                                        bool allow_assign) {
    parse::Parser parser(library, allow_assign);
    auto parsed = parser.parse_script(script);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    auto optimised = parse::optimise(std::move(parsed.value()));
    if (!optimised) {
        return std::unexpected(parse::ParseError{"optimise failed", 0});
    }
    ir::Compiled compiled = ir::Compiler::compile(*optimised);
    return Script(std::make_shared<ir::Compiled>(std::move(compiled)));
}

} // namespace fiber::script
