#ifndef FIBER_SCRIPT_SCRIPT_COMPILER_H
#define FIBER_SCRIPT_SCRIPT_COMPILER_H

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string_view>

#include "Library.h"
#include "Script.h"
#include "ast/Node.h"
#include "parse/ParseError.h"

namespace fiber::script {

enum class ScriptBackendMode : std::uint8_t {
    Interpreter = 0,
    PreferJit,
    RequireJit,
};

struct ScriptCompileOptions {
    bool allow_assign = true;
    std::size_t max_depth = kDefaultScriptMaxDepth;
    ScriptBackendMode backend = ScriptBackendMode::Interpreter;
};

std::expected<Script, parse::ParseError> compile_script(Library &library, std::string_view script,
                                                        const ScriptCompileOptions &options);

std::expected<Script, parse::ParseError> compile_script(Library &library, std::string_view script,
                                                        bool allow_assign = true,
                                                        std::size_t max_depth = kDefaultScriptMaxDepth);

// Compiles a config-level template string into a value-returning Script. `template_string` is a
// template-literal BODY (no surrounding backticks); its ${...} interpolations are parsed as
// template expressions and the evaluated (string-coerced) value becomes the script result.
// allow_assign defaults to false (a template value should not have assignment side effects).
//
// noexcept: the compile pipeline uses throwing allocations (std::string/std::vector/make_shared);
// this function catches std::bad_alloc / std::exception and converts them to a ParseError with a
// short SSO-safe literal message (the handler itself does not allocate).
//
// This does NOT enforce synchronicity. Callers evaluating via exec_sync must check
// Script::contains_async() themselves (exec_sync panics on async opcodes).
std::expected<Script, parse::ParseError>
compile_template_string(Library &library, std::string_view template_string, bool allow_assign = false,
                        std::size_t max_depth = kDefaultScriptMaxDepth) noexcept;

std::expected<Script, parse::ParseError> compile_template_string(Library &library, std::string_view template_string,
                                                                 const ScriptCompileOptions &options);

} // namespace fiber::script

#endif // FIBER_SCRIPT_SCRIPT_COMPILER_H
