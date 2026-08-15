#ifndef FIBER_SCRIPT_JIT_JIT_COMPILER_H
#define FIBER_SCRIPT_JIT_JIT_COMPILER_H

#include <cstdint>
#include <expected>
#include <memory>
#include <string>

#include <fiber/script/ir/Compiled.h>

namespace fiber::script::run {
class JitCode;
}

namespace fiber::script::jit {

enum class JitCompileStage : std::uint8_t {
    Unavailable = 0,
    Cfg,
    LlvmIr,
    Verify,
    Optimize,
    OrcAdd,
    StackMap,
    Lookup,
};

struct JitCompileError {
    JitCompileStage stage = JitCompileStage::Unavailable;
    std::string message;
    std::uint32_t pc = ir::Compiled::kNoPc;
};

[[nodiscard]] bool jit_is_available() noexcept;

std::expected<std::shared_ptr<const run::JitCode>, JitCompileError>
compile_jit(std::shared_ptr<const ir::Compiled> compiled);

} // namespace fiber::script::jit

#endif // FIBER_SCRIPT_JIT_JIT_COMPILER_H
