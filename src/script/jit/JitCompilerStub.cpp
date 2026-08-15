#include <fiber/script/jit/JitCompiler.h>

#if !FIBER_ENABLE_SCRIPT_JIT

namespace fiber::script::jit {

bool jit_is_available() noexcept { return false; }

std::expected<std::shared_ptr<const run::JitCode>, JitCompileError>
compile_jit(std::shared_ptr<const ir::Compiled> compiled) {
    (void) compiled;
    return std::unexpected(JitCompileError{JitCompileStage::Unavailable, "script JIT was disabled at build time",
                                           ir::Compiled::kNoPc});
}

} // namespace fiber::script::jit

#endif
