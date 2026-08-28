#include <fiber/script/ScriptCompiler.h>

#include <exception>
#include <new>
#include <string>
#include <utility>

#include "script/ir/Compiler.h"
#include "script/parse/Optimiser.h"
#include "script/parse/Parser.h"

namespace fiber::script {

std::expected<Script, parse::ParseError> compile_script(Library &library, std::string_view script, bool allow_assign,
                                                        std::size_t max_depth) {
    parse::Parser parser(library, allow_assign, max_depth);
    auto parsed = parser.parse_script(script);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    auto optimised = parse::optimise(std::move(parsed.value()));
    if (!optimised) {
        return std::unexpected(parse::ParseError{"optimise failed", 0});
    }
    auto compiled_result = ir::Compiler::compile(*optimised, max_depth);
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

std::expected<Script, parse::ParseError> compile_template_string(Library &library, std::string_view template_string,
                                                                 bool allow_assign, std::size_t max_depth) noexcept {
    try {
        parse::Parser parser(library, allow_assign, max_depth);
        auto tmpl = parser.parse_template(template_string);
        if (!tmpl) {
            return std::unexpected(tmpl.error());
        }
        auto compiled_result = ir::Compiler::compile_expression(*tmpl.value(), max_depth);
        if (!compiled_result) {
            const ir::CompileError &error = compiled_result.error();
            return std::unexpected(parse::ParseError{
                    error.message ? error.message : "compile failed",
                    error.position < 0 ? 0 : static_cast<std::size_t>(error.position),
            });
        }
        ir::Compiled compiled = std::move(compiled_result.value());
        return Script(std::make_shared<ir::Compiled>(std::move(compiled)));
    } catch (const std::bad_alloc &) {
        // Short SSO-safe literal: the handler itself does not allocate, so noexcept holds.
        return std::unexpected(parse::ParseError{"out of memory", 0});
    } catch (const std::exception &) {
        return std::unexpected(parse::ParseError{"compile failure", 0});
    }
}

} // namespace fiber::script
