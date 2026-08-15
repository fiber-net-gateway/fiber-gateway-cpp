#include <fiber/script/ScriptCompiler.h>

#include <exception>
#include <new>
#include <string>
#include <utility>

#include <fiber/script/ir/Compiler.h>
#include <fiber/script/jit/JitCompiler.h>
#include <fiber/script/parse/Optimiser.h>
#include <fiber/script/parse/Parser.h>

namespace fiber::script {

namespace {

std::expected<Script, parse::ParseError> make_script(ir::Compiled compiled, ScriptBackendMode backend) {
    auto compiled_ptr = std::make_shared<ir::Compiled>(std::move(compiled));
    if (backend == ScriptBackendMode::Interpreter) {
        return Script(std::move(compiled_ptr));
    }
    auto jit_code = jit::compile_jit(compiled_ptr);
    if (jit_code) {
        return Script(std::move(compiled_ptr), std::move(jit_code.value()));
    }
    if (backend == ScriptBackendMode::RequireJit) {
        std::size_t position = 0;
        if (jit_code.error().pc != ir::Compiled::kNoPc) {
            std::uint32_t source_position = compiled_ptr->find_position(jit_code.error().pc);
            if (source_position != ir::Compiled::kNoPc) {
                position = source_position;
            }
        }
        return std::unexpected(parse::ParseError{"JIT compile failed: " + jit_code.error().message, position});
    }
    auto diagnostic = std::make_shared<jit::JitCompileError>(std::move(jit_code.error()));
    return Script(std::move(compiled_ptr), std::move(diagnostic));
}

} // namespace

std::expected<Script, parse::ParseError> compile_script(Library &library, std::string_view script,
                                                        const ScriptCompileOptions &options) {
    parse::Parser parser(library, options.allow_assign, options.max_depth);
    auto parsed = parser.parse_script(script);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    auto optimised = parse::optimise(std::move(parsed.value()));
    if (!optimised) {
        return std::unexpected(parse::ParseError{"optimise failed", 0});
    }
    auto compiled_result = ir::Compiler::compile(*optimised, options.max_depth);
    if (!compiled_result) {
        const ir::CompileError &error = compiled_result.error();
        return std::unexpected(parse::ParseError{
                error.message ? error.message : "compile failed",
                error.position < 0 ? 0 : static_cast<std::size_t>(error.position),
        });
    }
    return make_script(std::move(compiled_result.value()), options.backend);
}

std::expected<Script, parse::ParseError> compile_script(Library &library, std::string_view script, bool allow_assign,
                                                        std::size_t max_depth) {
    return compile_script(library, script,
                          ScriptCompileOptions{allow_assign, max_depth, ScriptBackendMode::Interpreter});
}

std::expected<Script, parse::ParseError> compile_template_string(Library &library, std::string_view template_string,
                                                                 const ScriptCompileOptions &options) {
    parse::Parser parser(library, options.allow_assign, options.max_depth);
    auto tmpl = parser.parse_template(template_string);
    if (!tmpl) {
        return std::unexpected(tmpl.error());
    }
    auto compiled_result = ir::Compiler::compile_expression(*tmpl.value(), options.max_depth);
    if (!compiled_result) {
        const ir::CompileError &error = compiled_result.error();
        return std::unexpected(parse::ParseError{
                error.message ? error.message : "compile failed",
                error.position < 0 ? 0 : static_cast<std::size_t>(error.position),
        });
    }
    return make_script(std::move(compiled_result.value()), options.backend);
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
