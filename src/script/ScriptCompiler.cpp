#include "ScriptCompiler.h"

#include <string>
#include <utility>

#include "ir/Compiler.h"
#include "parse/Optimiser.h"
#include "parse/Parser.h"

namespace fiber::script {

std::expected<Script, parse::ParseError> compile_script(Library &library, std::string_view script, bool allow_assign) {
    parse::Parser parser(library, allow_assign);
    auto parsed = parser.parse_script(script);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    auto optimised = parse::optimise(std::move(parsed.value()));
    if (!optimised) {
        return std::unexpected(parse::ParseError{"optimise failed", 0});
    }
    auto compiled_result = ir::Compiler::compile(*optimised);
    if (!compiled_result) {
        const ir::CompileError &error = compiled_result.error();
        return std::unexpected(parse::ParseError{
                error.message ? error.message : "compile failed",
                error.position < 0 ? 0 : static_cast<std::size_t>(error.position),
        });
    }
    ir::Compiled compiled = std::move(compiled_result.value());
    return Script(std::make_shared<ir::Compiled>(std::move(compiled)));
}

} // namespace fiber::script
