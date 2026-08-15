#include <fiber/script/run/JitRuntime.h>

#include <cstdint>

#include <fiber/script/Library.h>
#include <fiber/script/gc/GcInternal.h>
#include <fiber/script/ir/Code.h>
#include <fiber/script/ir/Compiled.h>
#include <fiber/script/run/Access.h>
#include <fiber/script/run/Binaries.h>
#include <fiber/script/run/Compares.h>
#include <fiber/script/run/JitVm.h>
#include <fiber/script/run/Unaries.h>

namespace fiber::script::run {

namespace {

using RuntimeBinary = CallResult (*)(GcHeap &, ConstValueHandle, ConstValueHandle, ResultPayload &) noexcept;
using RuntimeUnary = CallResult (*)(GcHeap &, ConstValueHandle, ResultPayload &) noexcept;

class AnchorReset final {
public:
    explicit AnchorReset(JitFrameHeader *frame) noexcept : frame_(frame) {}
    ~AnchorReset() {
        if (frame_) {
            frame_->safepoint_return_pc = nullptr;
            frame_->safepoint_stack_pointer = nullptr;
        }
    }

private:
    JitFrameHeader *frame_ = nullptr;
};

ConstValueHandle handle(const JsValue *value) noexcept { return ConstValueHandle(const_cast<JsValue *>(value)); }

std::int64_t active_position(const JitFrameHeader &frame) noexcept {
    if (!frame.compiled) {
        return -1;
    }
    std::uint32_t position = frame.compiled->find_position(frame.active_pc);
    return position == ir::Compiled::kNoPc ? -1 : static_cast<std::int64_t>(position);
}

std::uint32_t abort_with(JitFrameHeader &frame, ScriptAbortReason reason, std::int64_t position = -1) noexcept {
    frame.abort = ScriptAbort{reason, position < 0 ? active_position(frame) : position};
    return static_cast<std::uint32_t>(JitStatus::Abort);
}

std::uint32_t apply_call_result(JitFrameHeader &frame, CallResult status, const ResultPayload &result,
                                JsValue *out) noexcept {
    switch (status) {
        case CallResult::Success:
            *out = result.value;
            return static_cast<std::uint32_t>(JitStatus::Success);
        case CallResult::Exception:
            *out = result.exception;
            return static_cast<std::uint32_t>(JitStatus::Exception);
        case CallResult::Abort: {
            std::int64_t position = result.abort.position;
            frame.abort = ScriptAbort{result.abort.reason, position < 0 ? active_position(frame) : position};
            return static_cast<std::uint32_t>(JitStatus::Abort);
        }
    }
    return abort_with(frame, ScriptAbortReason::Internal);
}

std::uint32_t apply_abi_result(JitFrameHeader &frame, const AbiResult &result, JsValue *out) noexcept {
    if (result.is_success()) {
        *out = result.value();
        return static_cast<std::uint32_t>(JitStatus::Success);
    }
    if (result.is_exception()) {
        *out = result.exception();
        return static_cast<std::uint32_t>(JitStatus::Exception);
    }
    std::int64_t position = result.abort().position;
    frame.abort = ScriptAbort{result.abort().reason, position < 0 ? active_position(frame) : position};
    return static_cast<std::uint32_t>(JitStatus::Abort);
}

RuntimeBinary binary_for(std::uint8_t opcode) noexcept {
    switch (opcode) {
        case ir::Code::BOP_PLUS:
            return &Binaries::plus;
        case ir::Code::BOP_MINUS:
            return &Binaries::minus;
        case ir::Code::BOP_MULTIPLY:
            return &Binaries::multiply;
        case ir::Code::BOP_DIVIDE:
            return &Binaries::divide;
        case ir::Code::BOP_MOD:
            return &Binaries::modulo;
        case ir::Code::BOP_MATCH:
            return &Binaries::matches;
        case ir::Code::BOP_LT:
            return &Binaries::lt;
        case ir::Code::BOP_LTE:
            return &Binaries::lte;
        case ir::Code::BOP_GT:
            return &Binaries::gt;
        case ir::Code::BOP_GTE:
            return &Binaries::gte;
        case ir::Code::BOP_EQ:
            return &Binaries::eq;
        case ir::Code::BOP_SEQ:
            return &Binaries::seq;
        case ir::Code::BOP_NE:
            return &Binaries::ne;
        case ir::Code::BOP_SNE:
            return &Binaries::sne;
        case ir::Code::BOP_IN:
            return &Binaries::in;
        case ir::Code::EXP_OBJECT:
            return &Access::expand_object;
        case ir::Code::EXP_ARRAY:
            return &Access::expand_array;
        case ir::Code::PUSH_ARRAY:
            return &Access::push_array;
        case ir::Code::IDX_GET:
            return &Access::index_get;
        default:
            return nullptr;
    }
}

RuntimeUnary unary_for(std::uint8_t opcode) noexcept {
    switch (opcode) {
        case ir::Code::UNARY_PLUS:
            return &Unaries::plus;
        case ir::Code::UNARY_MINUS:
            return &Unaries::minus;
        case ir::Code::UNARY_NEG:
            return &Unaries::neg;
        case ir::Code::UNARY_TYPEOF:
            return &Unaries::typeof_op;
        case ir::Code::ITERATE_INTO:
            return &Unaries::iterate;
        default:
            return nullptr;
    }
}

bool function_call(std::uint8_t opcode) noexcept {
    return opcode == ir::Code::CALL_FUNC || opcode == ir::Code::CALL_ASYNC_FUNC;
}

bool spread_call(std::uint8_t opcode) noexcept {
    return opcode == ir::Code::CALL_FUNC_SPREAD || opcode == ir::Code::CALL_ASYNC_FUNC_SPREAD;
}

bool async_call(std::uint8_t opcode) noexcept {
    return opcode == ir::Code::CALL_ASYNC_FUNC || opcode == ir::Code::CALL_ASYNC_FUNC_SPREAD ||
           opcode == ir::Code::CALL_ASYNC_CONST;
}

Library::Arguments call_arguments(std::uint8_t opcode, const JsValue *arguments, std::uint32_t argc) noexcept {
    if (spread_call(opcode)) {
        if (!arguments || js_value_type(arguments[0]) != JsNodeType::Array) {
            return {};
        }
        auto *array = js_value_heap_ptr<const GcArray>(arguments[0]);
        if (!array || array->size == 0) {
            return {};
        }
        return Library::Arguments{ConstValueHandle(array->elems), static_cast<std::uint32_t>(array->size)};
    }
    return Library::Arguments{argc == 0 ? nullptr : handle(arguments), argc};
}

std::uint32_t dispatch_host_call(JitFrameHeader &frame, JitVm &vm, std::uint8_t opcode, std::uint32_t raw,
                                 const JsValue *arguments, std::uint32_t argc, JsValue *out) noexcept {
    std::uint32_t function_index = function_call(opcode) ? raw >> 16u : raw >> 8u;
    const ir::Compiled::FuncConst &function = frame.compiled->func_const(function_index);
    Library::Arguments call_args = call_arguments(opcode, arguments, argc);
    Library::HostCallFrame &host_frame = vm.host_frame();
    if (!async_call(opcode)) {
        GcHeap::LocalMark mark(host_frame.runtime);
        if (opcode == ir::Code::CALL_CONST) {
            if (!function.sync_ct) {
                return abort_with(frame, ScriptAbortReason::InvalidState);
            }
            return apply_abi_result(frame, function.sync_ct(function.user_data, host_frame), out);
        }
        if (!function.sync_func) {
            return abort_with(frame, ScriptAbortReason::InvalidState);
        }
        return apply_abi_result(frame, function.sync_func(function.user_data, host_frame, call_args), out);
    }

    if (!vm.prepare_async_arguments(call_args.args.get(), call_args.argc)) {
        return abort_with(frame, ScriptAbortReason::OutOfMemory);
    }
    Library::Arguments persistent_args{frame.async_arguments, frame.async_argument_count};
    AsyncTask &task = vm.runtime_async_task();
    if (opcode == ir::Code::CALL_ASYNC_CONST) {
        if (!function.async_ct) {
            return abort_with(frame, ScriptAbortReason::InvalidState);
        }
        task = function.async_ct(function.user_data, host_frame);
    } else {
        if (!function.async_func) {
            return abort_with(frame, ScriptAbortReason::InvalidState);
        }
        task = function.async_func(function.user_data, host_frame, persistent_args);
    }
    if (!task.valid()) {
        return abort_with(frame,
                          task.allocation_failed() ? ScriptAbortReason::OutOfMemory : ScriptAbortReason::InvalidState);
    }
    vm.arm_async_task();
    frame.state = JitRunState::Suspend;
    return static_cast<std::uint32_t>(JitStatus::Suspend);
}

} // namespace

extern "C" std::uint32_t fiber_script_jit_runtime_call_impl(JitFrameHeader *frame, std::uint32_t opcode_value,
                                                            std::uint32_t raw, const JsValue *arguments,
                                                            std::uint32_t argc, JsValue *out) noexcept {
    AnchorReset reset(frame);
    if (!frame || !frame->compiled || !frame->vm_context || !out || frame->abi_version != kJitAbiVersion ||
        static_cast<std::uint8_t>(raw & 0xFFu) != static_cast<std::uint8_t>(opcode_value)) {
        if (frame) {
            return abort_with(*frame, ScriptAbortReason::InvalidState);
        }
        return static_cast<std::uint32_t>(JitStatus::Abort);
    }
    auto opcode = static_cast<std::uint8_t>(opcode_value);
    auto &vm = *static_cast<JitVm *>(frame->vm_context);
    GcHeap &heap = vm.host_frame().runtime;
    ResultPayload result;

    if (RuntimeBinary binary = binary_for(opcode)) {
        if (!arguments || argc != 2) {
            return abort_with(*frame, ScriptAbortReason::InvalidState);
        }
        return apply_call_result(*frame, binary(heap, handle(arguments), handle(arguments + 1), result), result, out);
    }
    if (RuntimeUnary unary = unary_for(opcode)) {
        if (!arguments || argc != 1) {
            return abort_with(*frame, ScriptAbortReason::InvalidState);
        }
        return apply_call_result(*frame, unary(heap, handle(arguments), result), result, out);
    }

    switch (opcode) {
        case ir::Code::NEW_OBJECT:
            *out = JsValue::make_object(heap.heap(), 0);
            if (js_value_type(*out) != JsNodeType::Object) {
                return abort_with(*frame, ScriptAbortReason::OutOfMemory);
            }
            return static_cast<std::uint32_t>(JitStatus::Success);
        case ir::Code::NEW_ARRAY:
            *out = JsValue::make_array(heap.heap(), 0);
            if (js_value_type(*out) != JsNodeType::Array) {
                return abort_with(*frame, ScriptAbortReason::OutOfMemory);
            }
            return static_cast<std::uint32_t>(JitStatus::Success);
        case ir::Code::IDX_SET:
        case ir::Code::IDX_SET_1: {
            if (!arguments || argc != 3) {
                return abort_with(*frame, ScriptAbortReason::InvalidState);
            }
            CallResult status = opcode == ir::Code::IDX_SET
                                        ? Access::index_set(heap, handle(arguments), handle(arguments + 1),
                                                            handle(arguments + 2), result)
                                        : Access::index_set1(heap, handle(arguments), handle(arguments + 1),
                                                             handle(arguments + 2), result);
            return apply_call_result(*frame, status, result, out);
        }
        case ir::Code::PROP_GET: {
            if (!arguments || argc != 1) {
                return abort_with(*frame, ScriptAbortReason::InvalidState);
            }
            ConstValueHandle key = handle(&frame->compiled->constant(raw >> 8u));
            return apply_call_result(*frame, Access::prop_get(heap, handle(arguments), key, result), result, out);
        }
        case ir::Code::PROP_SET:
        case ir::Code::PROP_SET_1: {
            if (!arguments || argc != 2) {
                return abort_with(*frame, ScriptAbortReason::InvalidState);
            }
            ConstValueHandle key = handle(&frame->compiled->constant(raw >> 8u));
            CallResult status =
                    opcode == ir::Code::PROP_SET
                            ? Access::prop_set(heap, handle(arguments), handle(arguments + 1), key, result)
                            : Access::prop_set1(heap, handle(arguments), handle(arguments + 1), key, result);
            return apply_call_result(*frame, status, result, out);
        }
        case ir::Code::ITERATE_NEXT: {
            if (!arguments || argc != 1) {
                return abort_with(*frame, ScriptAbortReason::InvalidState);
            }
            GcIterStep step = gc_iterator_next(&heap.heap(), ValueHandle(const_cast<JsValue *>(arguments)));
            if (step == GcIterStep::Mutated) {
                *out = JsValue::make_exception(ExceptionKind::IterationError);
                return static_cast<std::uint32_t>(JitStatus::Exception);
            }
            *out = JsValue::make_boolean(step == GcIterStep::Item);
            return static_cast<std::uint32_t>(JitStatus::Success);
        }
        case ir::Code::CALL_FUNC:
        case ir::Code::CALL_FUNC_SPREAD:
        case ir::Code::CALL_ASYNC_FUNC:
        case ir::Code::CALL_ASYNC_FUNC_SPREAD:
        case ir::Code::CALL_CONST:
        case ir::Code::CALL_ASYNC_CONST:
            return dispatch_host_call(*frame, vm, opcode, raw, arguments, argc, out);
        default:
            return abort_with(*frame, ScriptAbortReason::InvalidOpcode);
    }
}

#if defined(__x86_64__)
extern "C" __attribute__((naked)) std::uint32_t fiber_script_jit_runtime_call(JitFrameHeader *, std::uint32_t,
                                                                              std::uint32_t, const JsValue *,
                                                                              std::uint32_t, JsValue *) noexcept {
    __asm__ volatile("movq (%rsp), %rax\n\t"
                     "movq %rax, 0(%rdi)\n\t"
                     "leaq 8(%rsp), %rax\n\t"
                     "movq %rax, 8(%rdi)\n\t"
                     "jmp fiber_script_jit_runtime_call_impl\n\t");
}
#elif defined(__aarch64__)
extern "C" __attribute__((naked)) std::uint32_t fiber_script_jit_runtime_call(JitFrameHeader *, std::uint32_t,
                                                                              std::uint32_t, const JsValue *,
                                                                              std::uint32_t, JsValue *) noexcept {
    __asm__ volatile("str x30, [x0, #0]\n\t"
                     "mov x9, sp\n\t"
                     "str x9, [x0, #8]\n\t"
                     "b fiber_script_jit_runtime_call_impl\n\t");
}
#else
extern "C" std::uint32_t fiber_script_jit_runtime_call(JitFrameHeader *frame, std::uint32_t opcode, std::uint32_t raw,
                                                       const JsValue *arguments, std::uint32_t argc,
                                                       JsValue *out) noexcept {
    return fiber_script_jit_runtime_call_impl(frame, opcode, raw, arguments, argc, out);
}
#endif

extern "C" std::uint32_t fiber_script_jit_logic(const JsValue *value) noexcept {
    return value && Compares::logic(handle(value)) ? 1u : 0u;
}

extern "C" void fiber_script_jit_iterator_value(const JsValue *iterator_value, std::uint32_t key,
                                                JsValue *out) noexcept {
    if (!out) {
        return;
    }
    *out = JsValue::make_undefined();
    if (!iterator_value) {
        return;
    }
    auto *iterator = js_value_heap_ptr<const GcIterator>(*iterator_value);
    if (iterator && iterator->has_current) {
        *out = key ? iterator->current_key : iterator->current_value;
    }
}

} // namespace fiber::script::run
