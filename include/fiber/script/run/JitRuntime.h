#ifndef FIBER_SCRIPT_RUN_JIT_RUNTIME_H
#define FIBER_SCRIPT_RUN_JIT_RUNTIME_H

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <fiber/script/AsyncTask.h>
#include <fiber/script/JsValue.h>
#include <fiber/script/ScriptResult.h>

namespace fiber::script::ir {
class Compiled;
}

namespace fiber::script::run {

inline constexpr std::uint32_t kJitAbiVersion = 1;

enum class JitStatus : std::uint32_t {
    Success = 0,
    Exception,
    Abort,
    Suspend,
    SuccessVoid,
};

enum class JitRunState : std::uint32_t {
    Init = 0,
    Running,
    Suspend,
    AsyncValue,
    AsyncException,
    AsyncAbort,
    Success,
    SuccessVoid,
    Exception,
    Abort,
};

struct alignas(16) JitFrameHeader {
    // The architecture trampoline writes these two fields before entering any
    // runtime helper which may collect. Keep them first and pointer-aligned.
    const void *safepoint_return_pc = nullptr;
    const void *safepoint_stack_pointer = nullptr;

    void *vm_context = nullptr;
    const ir::Compiled *compiled = nullptr;
    JsValue *async_values = nullptr;
    JsValue *async_arguments = nullptr;
    JsValue *root = nullptr;
    std::uint32_t async_value_count = 0;
    std::uint32_t async_argument_count = 0;
    std::uint32_t resume_id = 0;
    std::uint32_t active_pc = 0;
    JitRunState state = JitRunState::Init;
    std::uint32_t abi_version = kJitAbiVersion;
    ScriptAbort abort{};
    JsValue pending_value = JsValue::make_undefined();
    JsValue pending_exception = JsValue::make_undefined();
};

using JitEntry = std::uint32_t (*)(JitFrameHeader *frame) noexcept;

static_assert(std::is_standard_layout_v<JitFrameHeader>);
static_assert(offsetof(JitFrameHeader, safepoint_return_pc) == 0);
static_assert(offsetof(JitFrameHeader, safepoint_stack_pointer) == sizeof(void *));
static_assert(alignof(JitFrameHeader) == 16);

extern "C" std::uint32_t fiber_script_jit_runtime_call(JitFrameHeader *frame, std::uint32_t opcode, std::uint32_t raw,
                                                       const JsValue *arguments, std::uint32_t argc,
                                                       JsValue *out) noexcept;

extern "C" std::uint32_t fiber_script_jit_runtime_call_impl(JitFrameHeader *frame, std::uint32_t opcode,
                                                            std::uint32_t raw, const JsValue *arguments,
                                                            std::uint32_t argc, JsValue *out) noexcept;

extern "C" std::uint32_t fiber_script_jit_logic(const JsValue *value) noexcept;

// Exact operator entry points used by generated code. Only BOP_PLUS may collect
// and therefore has an architecture trampoline plus a separate implementation.
// The remaining helpers are NoGC slow paths for values that miss native IR tag
// guards.
extern "C" std::uint32_t fiber_script_jit_bop_plus(JitFrameHeader *frame, const JsValue *arguments,
                                                   JsValue *out) noexcept;
extern "C" std::uint32_t fiber_script_jit_bop_plus_impl(JitFrameHeader *frame, const JsValue *arguments,
                                                        JsValue *out) noexcept;
extern "C" std::uint32_t fiber_script_jit_bop_minus(JitFrameHeader *frame, const JsValue *arguments,
                                                    JsValue *out) noexcept;
extern "C" std::uint32_t fiber_script_jit_bop_multiply(JitFrameHeader *frame, const JsValue *arguments,
                                                       JsValue *out) noexcept;
extern "C" std::uint32_t fiber_script_jit_bop_divide(JitFrameHeader *frame, const JsValue *arguments,
                                                     JsValue *out) noexcept;
extern "C" std::uint32_t fiber_script_jit_bop_modulo(JitFrameHeader *frame, const JsValue *arguments,
                                                     JsValue *out) noexcept;
extern "C" std::uint32_t fiber_script_jit_bop_matches(JitFrameHeader *frame, const JsValue *arguments,
                                                      JsValue *out) noexcept;
extern "C" std::uint32_t fiber_script_jit_bop_lt(JitFrameHeader *frame, const JsValue *arguments,
                                                 JsValue *out) noexcept;
extern "C" std::uint32_t fiber_script_jit_bop_lte(JitFrameHeader *frame, const JsValue *arguments,
                                                  JsValue *out) noexcept;
extern "C" std::uint32_t fiber_script_jit_bop_gt(JitFrameHeader *frame, const JsValue *arguments,
                                                 JsValue *out) noexcept;
extern "C" std::uint32_t fiber_script_jit_bop_gte(JitFrameHeader *frame, const JsValue *arguments,
                                                  JsValue *out) noexcept;
extern "C" std::uint32_t fiber_script_jit_bop_eq(JitFrameHeader *frame, const JsValue *arguments,
                                                 JsValue *out) noexcept;
extern "C" std::uint32_t fiber_script_jit_bop_seq(JitFrameHeader *frame, const JsValue *arguments,
                                                  JsValue *out) noexcept;
extern "C" std::uint32_t fiber_script_jit_bop_ne(JitFrameHeader *frame, const JsValue *arguments,
                                                 JsValue *out) noexcept;
extern "C" std::uint32_t fiber_script_jit_bop_sne(JitFrameHeader *frame, const JsValue *arguments,
                                                  JsValue *out) noexcept;
extern "C" std::uint32_t fiber_script_jit_bop_in(JitFrameHeader *frame, const JsValue *arguments,
                                                 JsValue *out) noexcept;

extern "C" std::uint32_t fiber_script_jit_unary_plus(JitFrameHeader *frame, const JsValue *argument,
                                                     JsValue *out) noexcept;
extern "C" std::uint32_t fiber_script_jit_unary_minus(JitFrameHeader *frame, const JsValue *argument,
                                                      JsValue *out) noexcept;
extern "C" std::uint32_t fiber_script_jit_unary_neg(JitFrameHeader *frame, const JsValue *argument,
                                                    JsValue *out) noexcept;
extern "C" std::uint32_t fiber_script_jit_unary_typeof(JitFrameHeader *frame, const JsValue *argument,
                                                       JsValue *out) noexcept;
extern "C" std::uint32_t fiber_script_jit_iterate_next(JitFrameHeader *frame, const JsValue *argument,
                                                       JsValue *out) noexcept;

} // namespace fiber::script::run

#endif // FIBER_SCRIPT_RUN_JIT_RUNTIME_H
