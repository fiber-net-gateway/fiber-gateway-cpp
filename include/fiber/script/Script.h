#ifndef FIBER_SCRIPT_SCRIPT_H
#define FIBER_SCRIPT_SCRIPT_H

#include <cstdint>
#include <memory>

#include "../async/Task.h"
#include "JsValue.h"
#include "ScriptResult.h"
#include "ir/Compiled.h"

namespace fiber::script {

class GcHeap;
namespace run {
class JitCode;
}
namespace jit {
struct JitCompileError;
}

class Script {
public:
    Script() = default;
    explicit Script(std::shared_ptr<ir::Compiled> compiled);
    Script(std::shared_ptr<ir::Compiled> compiled, std::shared_ptr<const run::JitCode> jit_code);
    Script(std::shared_ptr<ir::Compiled> compiled, std::shared_ptr<const jit::JitCompileError> jit_error);

    fiber::async::Task<ScriptResult> exec_async(fiber::script::JsValue root, void *attach, fiber::script::GcHeap &heap);

    ScriptResult exec_sync(fiber::script::JsValue root, void *attach, fiber::script::GcHeap &heap) const;

    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(compiled_); }
    [[nodiscard]] bool contains_async() const noexcept;
    [[nodiscard]] bool uses_jit() const noexcept { return static_cast<bool>(jit_code_); }
    [[nodiscard]] std::uint32_t jit_inlined_operator_helper_count() const noexcept;
    [[nodiscard]] const jit::JitCompileError *jit_compile_error() const noexcept { return jit_error_.get(); }

private:
    std::shared_ptr<ir::Compiled> compiled_;
    std::shared_ptr<const run::JitCode> jit_code_;
    std::shared_ptr<const jit::JitCompileError> jit_error_;
};

} // namespace fiber::script

#endif // FIBER_SCRIPT_SCRIPT_H
