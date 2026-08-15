#include <fiber/script/run/JitRuntime.h>

#include <cstdint>

#include <fiber/script/run/Binaries.h>
#include <fiber/script/run/JitVm.h>
#include <fiber/script/run/Unaries.h>

namespace fiber::script::run {

namespace {

ConstValueHandle handle(const JsValue *value) noexcept { return ConstValueHandle(const_cast<JsValue *>(value)); }

std::uint32_t apply_operator_result(JitFrameHeader &frame, CallResult status, const ResultPayload &result,
                                    JsValue &out) noexcept {
    switch (status) {
        case CallResult::Success:
            out = result.value;
            return static_cast<std::uint32_t>(JitStatus::Success);
        case CallResult::Exception:
            out = result.exception;
            return static_cast<std::uint32_t>(JitStatus::Exception);
        case CallResult::Abort:
            frame.abort = result.abort;
            if (frame.abort.reason == ScriptAbortReason::None) {
                frame.abort = ScriptAbort{ScriptAbortReason::Internal, -1};
            }
            return static_cast<std::uint32_t>(JitStatus::Abort);
    }
    frame.abort = ScriptAbort{ScriptAbortReason::Internal, -1};
    return static_cast<std::uint32_t>(JitStatus::Abort);
}

template<CallResult (*Operation)(GcHeap &, ConstValueHandle, ConstValueHandle, ResultPayload &) noexcept>
std::uint32_t exact_binary(JitFrameHeader *frame, const JsValue *arguments, JsValue *out) noexcept {
    // Generated code establishes this trusted ABI at entry. Keeping validation
    // out of each NoGC operator lets the imported bitcode collapse to SSA.
    auto &vm = *static_cast<JitVm *>(frame->vm_context);
    ResultPayload result;
    CallResult status = Operation(vm.host_frame().runtime, handle(arguments), handle(arguments + 1), result);
    return apply_operator_result(*frame, status, result, *out);
}

template<CallResult (*Operation)(GcHeap &, ConstValueHandle, ResultPayload &) noexcept>
std::uint32_t exact_unary(JitFrameHeader *frame, const JsValue *argument, JsValue *out) noexcept {
    auto &vm = *static_cast<JitVm *>(frame->vm_context);
    ResultPayload result;
    CallResult status = Operation(vm.host_frame().runtime, handle(argument), result);
    return apply_operator_result(*frame, status, result, *out);
}

} // namespace

extern "C" const std::uint32_t fiber_script_jit_operator_bitcode_version = kJitOperatorBitcodeVersion;
extern "C" const std::uint32_t fiber_script_jit_operator_abi_version = kJitAbiVersion;
extern "C" const std::uint64_t fiber_script_jit_operator_layout_fingerprint = kJitOperatorLayoutFingerprint;

#define FIBER_JIT_BINARY_HELPER(symbol, operation)                                                                     \
    extern "C" std::uint32_t symbol(JitFrameHeader *frame, const JsValue *arguments, JsValue *out) noexcept {          \
        return exact_binary<&Binaries::operation>(frame, arguments, out);                                              \
    }

FIBER_JIT_BINARY_HELPER(fiber_script_jit_bop_minus, minus)
FIBER_JIT_BINARY_HELPER(fiber_script_jit_bop_multiply, multiply)
FIBER_JIT_BINARY_HELPER(fiber_script_jit_bop_divide, divide)
FIBER_JIT_BINARY_HELPER(fiber_script_jit_bop_modulo, modulo)
FIBER_JIT_BINARY_HELPER(fiber_script_jit_bop_matches, matches)
FIBER_JIT_BINARY_HELPER(fiber_script_jit_bop_lt, lt)
FIBER_JIT_BINARY_HELPER(fiber_script_jit_bop_lte, lte)
FIBER_JIT_BINARY_HELPER(fiber_script_jit_bop_gt, gt)
FIBER_JIT_BINARY_HELPER(fiber_script_jit_bop_gte, gte)
FIBER_JIT_BINARY_HELPER(fiber_script_jit_bop_eq, eq)
FIBER_JIT_BINARY_HELPER(fiber_script_jit_bop_seq, seq)
FIBER_JIT_BINARY_HELPER(fiber_script_jit_bop_ne, ne)
FIBER_JIT_BINARY_HELPER(fiber_script_jit_bop_sne, sne)
FIBER_JIT_BINARY_HELPER(fiber_script_jit_bop_in, in)

#undef FIBER_JIT_BINARY_HELPER

#define FIBER_JIT_UNARY_HELPER(symbol, operation)                                                                      \
    extern "C" std::uint32_t symbol(JitFrameHeader *frame, const JsValue *argument, JsValue *out) noexcept {           \
        return exact_unary<&Unaries::operation>(frame, argument, out);                                                 \
    }

FIBER_JIT_UNARY_HELPER(fiber_script_jit_unary_plus, plus)
FIBER_JIT_UNARY_HELPER(fiber_script_jit_unary_minus, minus)
FIBER_JIT_UNARY_HELPER(fiber_script_jit_unary_neg, neg)
FIBER_JIT_UNARY_HELPER(fiber_script_jit_unary_typeof, typeof_op)

#undef FIBER_JIT_UNARY_HELPER

} // namespace fiber::script::run
