#include "InterpreterVm.h"

#include <new>
#include <string>
#include <utility>

#include "../../common/Assert.h"
#include "../Library.h"
#include "../gc/GcInternal.h"
#include "Access.h"
#include "Binaries.h"
#include "Compares.h"
#include "Unaries.h"

namespace fiber::script::run {

namespace {

ScriptResult make_abort(ScriptAbortReason reason, std::int64_t position = -1) {
    return ScriptResult::abort(reason, position);
}

ScriptResult make_oom(std::int64_t position) { return make_abort(ScriptAbortReason::OutOfMemory, position); }

bool opcode_uses_spread(std::uint8_t op) {
    return op == ir::Code::CALL_FUNC_SPREAD || op == ir::Code::CALL_ASYNC_FUNC_SPREAD;
}

bool opcode_is_function_call(std::uint8_t op) { return op == ir::Code::CALL_FUNC || op == ir::Code::CALL_ASYNC_FUNC; }

bool opcode_is_async_call(std::uint8_t op) {
    return op == ir::Code::CALL_ASYNC_FUNC || op == ir::Code::CALL_ASYNC_FUNC_SPREAD ||
           op == ir::Code::CALL_ASYNC_CONST;
}

ConstValueHandle const_handle(const fiber::script::JsValue &value) noexcept {
    return ConstValueHandle(const_cast<fiber::script::JsValue *>(&value));
}

std::int64_t position_at(const ir::Compiled &compiled, std::size_t pc) noexcept {
    if (pc > UINT32_MAX) {
        return -1;
    }
    std::uint32_t position = compiled.find_position(static_cast<std::uint32_t>(pc));
    return position == ir::Compiled::kNoPc ? -1 : static_cast<std::int64_t>(position);
}

} // namespace

InterpreterVm::InterpreterVm(const ir::Compiled &compiled, const fiber::script::JsValue &root, void *attach,
                             GcHeap &runtime) :
    compile_(compiled), root_(root), attach_(attach), runtime_(runtime), reg_(runtime_.roots(), *this) {
    std::size_t total = compile_.stack_size() + compile_.var_table_size();
    if (total > 0) {
        slots_.reset(new (std::nothrow) fiber::script::JsValue[total]);
        if (!slots_) {
            state_ = State::Abort;
            result_.abort = ScriptAbort{ScriptAbortReason::OutOfMemory, -1};
            return;
        }
        stack_ = slots_.get();
        vars_ = stack_ + compile_.stack_size();
    }
}

ScriptResult InterpreterVm::result() const noexcept {
    switch (state_) {
        case State::Success:
            return ScriptResult::success(result_.value);
        case State::Exception:
            return ScriptResult::exception(result_.exception);
        case State::Abort:
            return ScriptResult::abort(result_.abort.reason, result_.abort.position);
        case State::Init:
        case State::Running:
        case State::Suspend:
        case State::AsyncRetSuc:
        case State::AsyncRetExp:
        case State::AsyncRetAbort:
            return ScriptResult::abort(ScriptAbortReason::None);
    }
    return ScriptResult::abort(ScriptAbortReason::Internal);
}

bool InterpreterVm::done() const noexcept {
    return state_ == State::Success || state_ == State::Exception || state_ == State::Abort;
}

void InterpreterVm::iterate() {
    if (done()) {
        return;
    }
    if (state_ == State::AsyncRetSuc || state_ == State::AsyncRetExp || state_ == State::AsyncRetAbort) {
        if (!apply_async_result()) {
            return;
        }
    }
    if (state_ == State::Suspend) {
        return;
    }
    state_ = State::Running;
    using BinaryOp = CallResult (*)(GcHeap &, ConstValueHandle, ConstValueHandle, ResultPayload &) noexcept;
    auto apply_binary = [&](BinaryOp op, std::size_t epc) {
        FIBER_ASSERT(sp_ >= 2);
        fiber::script::JsValue *lhs = &stack_[sp_ - 2];
        fiber::script::JsValue *rhs = &stack_[sp_ - 1];
        CallResult status = op(runtime_, lhs, rhs, result_);
        if (!handle_call_result(status, epc)) {
            return false;
        }
        if (status == CallResult::Success) {
            *lhs = result_.value;
            --sp_;
        }
        return true;
    };
    using UnaryOp = CallResult (*)(GcHeap &, ConstValueHandle, ResultPayload &) noexcept;
    auto apply_unary = [&](UnaryOp op, std::size_t epc) {
        FIBER_ASSERT(sp_ >= 1);
        fiber::script::JsValue *value = &stack_[sp_ - 1];
        CallResult status = op(runtime_, value, result_);
        if (!handle_call_result(status, epc)) {
            return false;
        }
        if (status == CallResult::Success) {
            *value = result_.value;
        }
        return true;
    };
    const std::int32_t *codes = compile_.codes();
    const std::uint32_t code_size = compile_.code_size();
    while (pc_ < code_size) {
        std::int32_t instr = codes[pc_++];
        std::uint32_t raw = static_cast<std::uint32_t>(instr);
        std::uint8_t op = static_cast<std::uint8_t>(raw & 0xFFu);
        switch (op) {
            case ir::Code::NOOP:
                break;
            case ir::Code::LOAD_CONST: {
                std::uint32_t idx = raw >> 8u;
                stack_[sp_++] = compile_.constant(idx);
                break;
            }
            case ir::Code::LOAD_ROOT:
                stack_[sp_++] = root_;
                break;
            case ir::Code::DUMP:
                stack_[sp_] = stack_[sp_ - 1];
                ++sp_;
                break;
            case ir::Code::POP:
                if (sp_ > 0) {
                    --sp_;
                }
                break;
            case ir::Code::LOAD_VAR:
                stack_[sp_++] = vars_[static_cast<std::size_t>(raw >> 8u)];
                break;
            case ir::Code::STORE_VAR:
                vars_[static_cast<std::size_t>(raw >> 8u)] = stack_[--sp_];
                break;
            case ir::Code::BOP_PLUS:
                if (!apply_binary(&Binaries::plus, pc_ - 1)) {
                    return;
                }
                break;
            case ir::Code::BOP_MINUS:
                if (!apply_binary(&Binaries::minus, pc_ - 1)) {
                    return;
                }
                break;
            case ir::Code::BOP_MULTIPLY:
                if (!apply_binary(&Binaries::multiply, pc_ - 1)) {
                    return;
                }
                break;
            case ir::Code::BOP_DIVIDE:
                if (!apply_binary(&Binaries::divide, pc_ - 1)) {
                    return;
                }
                break;
            case ir::Code::BOP_MOD:
                if (!apply_binary(&Binaries::modulo, pc_ - 1)) {
                    return;
                }
                break;
            case ir::Code::BOP_MATCH:
                if (!apply_binary(&Binaries::matches, pc_ - 1)) {
                    return;
                }
                break;
            case ir::Code::BOP_LT:
                if (!apply_binary(&Binaries::lt, pc_ - 1)) {
                    return;
                }
                break;
            case ir::Code::BOP_LTE:
                if (!apply_binary(&Binaries::lte, pc_ - 1)) {
                    return;
                }
                break;
            case ir::Code::BOP_GT:
                if (!apply_binary(&Binaries::gt, pc_ - 1)) {
                    return;
                }
                break;
            case ir::Code::BOP_GTE:
                if (!apply_binary(&Binaries::gte, pc_ - 1)) {
                    return;
                }
                break;
            case ir::Code::BOP_EQ:
                if (!apply_binary(&Binaries::eq, pc_ - 1)) {
                    return;
                }
                break;
            case ir::Code::BOP_SEQ:
                if (!apply_binary(&Binaries::seq, pc_ - 1)) {
                    return;
                }
                break;
            case ir::Code::BOP_NE:
                if (!apply_binary(&Binaries::ne, pc_ - 1)) {
                    return;
                }
                break;
            case ir::Code::BOP_SNE:
                if (!apply_binary(&Binaries::sne, pc_ - 1)) {
                    return;
                }
                break;
            case ir::Code::BOP_IN:
                if (!apply_binary(&Binaries::in, pc_ - 1)) {
                    return;
                }
                break;
            case ir::Code::UNARY_PLUS: {
                if (!apply_unary(&Unaries::plus, pc_ - 1)) {
                    return;
                }
            } break;
            case ir::Code::UNARY_MINUS: {
                if (!apply_unary(&Unaries::minus, pc_ - 1)) {
                    return;
                }
            } break;
            case ir::Code::UNARY_NEG: {
                if (!apply_unary(&Unaries::neg, pc_ - 1)) {
                    return;
                }
            } break;
            case ir::Code::UNARY_TYPEOF: {
                if (!apply_unary(&Unaries::typeof_op, pc_ - 1)) {
                    return;
                }
            } break;
            case ir::Code::NEW_OBJECT: {
                fiber::script::JsValue obj = fiber::script::JsValue::make_object(runtime_.heap(), 0);
                if (fiber::script::js_value_type(obj) != fiber::script::JsNodeType::Object) {
                    ScriptResult error = make_oom(position_at(compile_, pc_ - 1));
                    if (!handle_error(error, pc_ - 1)) {
                        return;
                    }
                    continue;
                }
                stack_[sp_++] = obj;
                break;
            }
            case ir::Code::NEW_ARRAY: {
                fiber::script::JsValue arr = fiber::script::JsValue::make_array(runtime_.heap(), 0);
                if (fiber::script::js_value_type(arr) != fiber::script::JsNodeType::Array) {
                    ScriptResult error = make_oom(position_at(compile_, pc_ - 1));
                    if (!handle_error(error, pc_ - 1)) {
                        return;
                    }
                    continue;
                }
                stack_[sp_++] = arr;
                break;
            }
            case ir::Code::EXP_OBJECT:
                if (!apply_binary(&Access::expand_object, pc_ - 1)) {
                    return;
                }
                break;
            case ir::Code::EXP_ARRAY:
                if (!apply_binary(&Access::expand_array, pc_ - 1)) {
                    return;
                }
                break;
            case ir::Code::PUSH_ARRAY:
                if (!apply_binary(&Access::push_array, pc_ - 1)) {
                    return;
                }
                break;
            case ir::Code::IDX_GET:
                if (!apply_binary(&Access::index_get, pc_ - 1)) {
                    return;
                }
                break;
            case ir::Code::IDX_SET: {
                FIBER_ASSERT(sp_ >= 3);
                fiber::script::JsValue *parent = &stack_[sp_ - 3];
                fiber::script::JsValue *key = &stack_[sp_ - 2];
                fiber::script::JsValue *value = &stack_[sp_ - 1];
                CallResult status = Access::index_set(runtime_, parent, key, value, result_);
                if (!handle_call_result(status, pc_ - 1)) {
                    return;
                }
                if (status == CallResult::Success) {
                    *parent = result_.value;
                    sp_ -= 2;
                }
                break;
            }
            case ir::Code::IDX_SET_1: {
                FIBER_ASSERT(sp_ >= 3);
                fiber::script::JsValue *parent = &stack_[sp_ - 3];
                fiber::script::JsValue *key = &stack_[sp_ - 2];
                fiber::script::JsValue *value = &stack_[sp_ - 1];
                CallResult status = Access::index_set1(runtime_, parent, key, value, result_);
                if (!handle_call_result(status, pc_ - 1)) {
                    return;
                }
                if (status == CallResult::Success) {
                    *parent = result_.value;
                    sp_ -= 2;
                }
                break;
            }
            case ir::Code::PROP_GET: {
                std::uint32_t idx = raw >> 8u;
                ConstValueHandle key = const_handle(compile_.constant(idx));
                fiber::script::JsValue *parent = &stack_[sp_ - 1];
                CallResult status = Access::prop_get(runtime_, parent, key, result_);
                if (!handle_call_result(status, pc_ - 1)) {
                    return;
                }
                parent[0] = result_.value;
                break;
            }
            case ir::Code::PROP_SET: {
                std::uint32_t idx = raw >> 8u;
                ConstValueHandle key = const_handle(compile_.constant(idx));
                FIBER_ASSERT(sp_ >= 2);
                fiber::script::JsValue *parent = &stack_[sp_ - 2];
                fiber::script::JsValue *value = &stack_[sp_ - 1];
                CallResult status = Access::prop_set(runtime_, parent, value, key, result_);
                if (!handle_call_result(status, pc_ - 1)) {
                    return;
                }
                if (status == CallResult::Success) {
                    *parent = result_.value;
                    --sp_;
                }
                break;
            }
            case ir::Code::PROP_SET_1: {
                std::uint32_t idx = raw >> 8u;
                ConstValueHandle key = const_handle(compile_.constant(idx));
                FIBER_ASSERT(sp_ >= 2);
                fiber::script::JsValue *parent = &stack_[sp_ - 2];
                fiber::script::JsValue *value = &stack_[sp_ - 1];
                CallResult status = Access::prop_set1(runtime_, parent, value, key, result_);
                if (!handle_call_result(status, pc_ - 1)) {
                    return;
                }
                if (status == CallResult::Success) {
                    *parent = result_.value;
                    --sp_;
                }
                break;
            }
            case ir::Code::CALL_FUNC:
            case ir::Code::CALL_FUNC_SPREAD:
            case ir::Code::CALL_ASYNC_FUNC:
            case ir::Code::CALL_ASYNC_FUNC_SPREAD:
            case ir::Code::CALL_CONST:
            case ir::Code::CALL_ASYNC_CONST: {
                std::uint32_t func_index = opcode_is_function_call(op) ? (raw >> 16u) : (raw >> 8u);
                std::uint32_t argc = opcode_is_function_call(op) ? ((raw >> 8u) & 0xFFu) : 0;
                if (!dispatch_func_const(op, compile_.func_const(func_index), argc)) {
                    return;
                }
                break;
            }
            case ir::Code::JUMP:
                pc_ = static_cast<std::size_t>(raw >> 8u);
                break;
            case ir::Code::JUMP_IF_FALSE: {
                fiber::script::JsValue *cond = &stack_[sp_ - 1];
                if (!Compares::logic(cond)) {
                    pc_ = static_cast<std::size_t>(raw >> 8u);
                }
                --sp_;
                break;
            }
            case ir::Code::JUMP_IF_TRUE: {
                fiber::script::JsValue *cond = &stack_[sp_ - 1];
                if (Compares::logic(cond)) {
                    pc_ = static_cast<std::size_t>(raw >> 8u);
                }
                --sp_;
                break;
            }
            case ir::Code::ITERATE_INTO: {
                std::size_t idx = static_cast<std::size_t>(raw >> kInstrumentLen);
                FIBER_ASSERT(sp_ >= 1);
                CallResult status = Unaries::iterate(runtime_, &stack_[sp_ - 1], result_);
                if (!handle_call_result(status, pc_ - 1)) {
                    return;
                }
                if (status == CallResult::Success) {
                    vars_[idx] = result_.value;
                    --sp_;
                }
                break;
            }
            case ir::Code::ITERATE_NEXT: {
                std::size_t idx = static_cast<std::size_t>(raw >> kInstrumentLen);
                fiber::script::GcHeap::LocalMark mark(runtime_);
                fiber::script::ValueHandle out = runtime_.local_value();
                if (!out) {
                    result_.abort = fiber::script::ScriptAbort{fiber::script::ScriptAbortReason::OutOfMemory, -1};
                    return;
                }
                bool done = true;
                bool ok = fiber::script::gc_iterator_next(&runtime_.heap(), fiber::script::ValueHandle(&vars_[idx]),
                                                          out, done);
                stack_[sp_++] = fiber::script::JsValue::make_boolean(ok && !done);
                break;
            }
            case ir::Code::ITERATE_KEY: {
                std::size_t var_idx = static_cast<std::size_t>((raw >> kInstrumentLen) & kMaxIteratorVar);
                std::size_t iter_idx = static_cast<std::size_t>(raw >> kIteratorOff);
                auto *iter = fiber::script::js_value_heap_ptr<fiber::script::GcIterator>(vars_[iter_idx]);
                if (iter && iter->has_current) {
                    vars_[var_idx] = iter->current_key;
                } else {
                    vars_[var_idx] = fiber::script::JsValue::make_undefined();
                }
                break;
            }
            case ir::Code::ITERATE_VALUE: {
                std::size_t var_idx = static_cast<std::size_t>((raw >> kInstrumentLen) & kMaxIteratorVar);
                std::size_t iter_idx = static_cast<std::size_t>(raw >> kIteratorOff);
                auto *iter = fiber::script::js_value_heap_ptr<fiber::script::GcIterator>(vars_[iter_idx]);
                if (iter && iter->has_current) {
                    vars_[var_idx] = iter->current_value;
                } else {
                    vars_[var_idx] = fiber::script::JsValue::make_undefined();
                }
                break;
            }
            case ir::Code::INTO_CATCH: {
                std::size_t idx = static_cast<std::size_t>(raw >> kInstrumentLen);
                vars_[idx] = result_.exception;
                result_.exception = fiber::script::JsValue::make_undefined();
                break;
            }
            case ir::Code::END_RETURN: {
                if (sp_ > 0) {
                    result_.value = stack_[sp_ - 1];
                } else {
                    result_.value = fiber::script::JsValue::make_undefined();
                }
                state_ = State::Success;
                return;
            }
            case ir::Code::THROW_EXP: {
                fiber::script::JsValue thrown = stack_[--sp_];
                ScriptResult error = ScriptResult::exception(thrown);
                if (!handle_error(error, pc_ - 1)) {
                    return;
                }
                break;
            }
            default: {
                ScriptResult error = make_abort(ScriptAbortReason::InvalidOpcode, position_at(compile_, pc_ - 1));
                if (!handle_error(error, pc_ - 1)) {
                    return;
                }
                break;
            }
        }
    }
    result_.value = fiber::script::JsValue::make_undefined();
    state_ = State::Success;
}

void InterpreterVm::async_complete(void *context, const ScriptResult &result) noexcept {
    auto *vm = static_cast<InterpreterVm *>(context);
    if (!vm || vm->done()) {
        return;
    }
    if (!vm->async_.valid() || vm->state_ != State::Suspend) {
        return;
    }
    if (result.is_success()) {
        vm->result_.value = result.value();
        vm->state_ = State::AsyncRetSuc;
        return;
    }
    if (result.is_exception()) {
        vm->result_.exception = result.exception();
        vm->state_ = State::AsyncRetExp;
        return;
    }
    vm->result_.abort = result.abort();
    vm->state_ = State::AsyncRetAbort;
}

void InterpreterVm::visit_roots(fiber::script::GcRootVisitor &visitor) noexcept {
    visitor.visit(&root_);
    visitor.visit_range(stack_, sp_);
    visitor.visit_range(vars_, compile_.var_table_size());
    if (state_ == State::Running || state_ == State::Suspend || state_ == State::AsyncRetSuc ||
        state_ == State::Success) {
        visitor.visit(&result_.value);
    }
    if (state_ == State::AsyncRetExp || state_ == State::Exception) {
        visitor.visit(&result_.exception);
    }
}

Library::HostCallFrame InterpreterVm::make_call_frame() const {
    Library::HostCallFrame frame;
    frame.runtime = const_cast<GcHeap *>(&runtime_);
    frame.root = const_cast<fiber::script::JsValue *>(&root_);
    frame.attach = attach_;
    return frame;
}

Library::Arguments InterpreterVm::make_call_arguments(std::uint8_t op, std::uint32_t encoded_argc,
                                                      std::size_t &arg_base) {
    arg_base = sp_;
    if (opcode_uses_spread(op)) {
        if (!stack_ || sp_ == 0) {
            return {};
        }
        const fiber::script::JsValue &args = stack_[sp_ - 1];
        if (fiber::script::js_value_type(args) != fiber::script::JsNodeType::Array) {
            return {};
        }
        auto *arr = fiber::script::js_value_heap_ptr<const fiber::script::GcArray>(args);
        if (!arr || arr->size == 0) {
            return {};
        }
        return Library::Arguments{fiber::script::ConstValueHandle(arr->elems), static_cast<std::uint32_t>(arr->size)};
    }
    if (opcode_is_function_call(op) && encoded_argc > 0) {
        FIBER_ASSERT(sp_ >= encoded_argc);
        arg_base = sp_ - encoded_argc;
        return Library::Arguments{fiber::script::ConstValueHandle(stack_ + arg_base), encoded_argc};
    }
    return Library::Arguments{nullptr, encoded_argc};
}

bool InterpreterVm::dispatch_func_const(std::uint8_t op, const ir::Compiled::FuncConst &func_const,
                                        std::uint32_t encoded_argc) {
    std::size_t arg_base = sp_;
    Library::Arguments arguments = make_call_arguments(op, encoded_argc, arg_base);
    const Library::HostCallFrame frame = make_call_frame();
    const std::size_t epc = pc_ - 1;
    switch (op) {
        case ir::Code::CALL_FUNC:
        case ir::Code::CALL_FUNC_SPREAD: {
            FIBER_ASSERT(func_const.sync_func);
            GcHeap::LocalMark mark(runtime_);
            ScriptResult result = func_const.sync_func(func_const.user_data, frame, arguments);
            return apply_call_result(result, op, encoded_argc, epc);
        }
        case ir::Code::CALL_CONST: {
            FIBER_ASSERT(func_const.sync_ct);
            GcHeap::LocalMark mark(runtime_);
            ScriptResult result = func_const.sync_ct(func_const.user_data, frame);
            return apply_call_result(result, op, encoded_argc, epc);
        }
        case ir::Code::CALL_ASYNC_CONST: {
            FIBER_ASSERT(func_const.async_ct);
            async_ = func_const.async_ct(func_const.user_data, frame);
            if (!async_.valid()) {
                return handle_error(ScriptResult::abort(async_.allocation_failed() ? ScriptAbortReason::OutOfMemory
                                                                                   : ScriptAbortReason::InvalidState),
                                    epc);
            }
            async_.set_completion({&InterpreterVm::async_complete, this});
            state_ = State::Suspend;
            return false;
        }
        case ir::Code::CALL_ASYNC_FUNC:
        case ir::Code::CALL_ASYNC_FUNC_SPREAD: {
            FIBER_ASSERT(func_const.async_func);
            async_ = func_const.async_func(func_const.user_data, frame, arguments);
            if (!async_.valid()) {
                if (op == ir::Code::CALL_ASYNC_FUNC) {
                    sp_ = arg_base;
                }
                return handle_error(ScriptResult::abort(async_.allocation_failed() ? ScriptAbortReason::OutOfMemory
                                                                                   : ScriptAbortReason::InvalidState),
                                    epc);
            }
            async_.set_completion({&InterpreterVm::async_complete, this});
            state_ = State::Suspend;
            return false;
        }
    }
    return true;
}

bool InterpreterVm::apply_call_result(const ScriptResult &result, std::uint8_t op, std::uint32_t argc,
                                      std::size_t epc) {
    if (result.is_success()) {
        switch (op) {
            case ir::Code::CALL_FUNC:
            case ir::Code::CALL_ASYNC_FUNC:
                FIBER_ASSERT(sp_ >= argc);
                sp_ -= argc;
                [[fallthrough]];
            case ir::Code::CALL_CONST:
            case ir::Code::CALL_ASYNC_CONST:
                if (sp_ < compile_.stack_size()) {
                    stack_[sp_] = result.value();
                }
                ++sp_;
                break;
            case ir::Code::CALL_FUNC_SPREAD:
            case ir::Code::CALL_ASYNC_FUNC_SPREAD:
                if (sp_ > 0 && sp_ - 1 < compile_.stack_size()) {
                    stack_[sp_ - 1] = result.value();
                }
                break;
            default:
                return handle_error(ScriptResult::abort(ScriptAbortReason::InvalidOpcode), epc);
        }
        return true;
    }
    if (op == ir::Code::CALL_FUNC || op == ir::Code::CALL_ASYNC_FUNC) {
        FIBER_ASSERT(sp_ >= argc);
        sp_ -= argc;
    }
    return handle_error(result, epc);
}

bool InterpreterVm::handle_call_result(CallResult status, std::size_t epc) {
    switch (status) {
        case CallResult::Success:
            return true;
        case CallResult::Exception:
            return handle_error(ScriptResult::exception(result_.exception), epc);
        case CallResult::Abort:
            return handle_error(ScriptResult::abort(result_.abort.reason, result_.abort.position), epc);
    }
    return handle_error(ScriptResult::abort(ScriptAbortReason::Internal), epc);
}

bool InterpreterVm::catch_for_exception(std::size_t epc) {
    std::uint32_t target =
            epc > UINT32_MAX ? ir::Compiled::kNoPc : compile_.find_catch(static_cast<std::uint32_t>(epc));
    sp_ = 0;
    if (target == ir::Compiled::kNoPc) {
        return false;
    }
    pc_ = static_cast<std::size_t>(target);
    state_ = State::Running;
    return true;
}

bool InterpreterVm::handle_error(ScriptResult error, std::size_t epc) {
    if (error.is_exception()) {
        result_.exception = error.exception();
        if (catch_for_exception(epc)) {
            return true;
        }
        state_ = State::Exception;
        return false;
    }
    std::int64_t position = position_at(compile_, epc);
    if (error.is_abort() && error.abort().position < 0 && position >= 0) {
        result_.abort = ScriptAbort{error.abort().reason, position};
    } else {
        result_.abort = error.abort();
    }
    state_ = State::Abort;
    return false;
}

bool InterpreterVm::apply_async_result() {
    if (state_ != State::AsyncRetSuc && state_ != State::AsyncRetExp && state_ != State::AsyncRetAbort) {
        return true;
    }
    if (!async_.valid() || pc_ == 0 || pc_ - 1 >= compile_.code_size()) {
        return handle_error(ScriptResult::abort(ScriptAbortReason::InvalidState), pc_ == 0 ? 0 : pc_ - 1);
    }

    const std::size_t epc = pc_ - 1;
    const std::uint32_t raw = static_cast<std::uint32_t>(compile_.codes()[epc]);
    const std::uint8_t op = static_cast<std::uint8_t>(raw & 0xFFu);
    if (!opcode_is_async_call(op)) {
        async_.reset();
        state_ = State::Running;
        return handle_error(ScriptResult::abort(ScriptAbortReason::InvalidOpcode), epc);
    }

    const std::uint32_t argc = opcode_is_function_call(op) ? ((raw >> 8u) & 0xFFu) : 0;
    ScriptResult result;
    switch (state_) {
        case State::AsyncRetSuc:
            result = ScriptResult::success(result_.value);
            break;
        case State::AsyncRetExp:
            result = ScriptResult::exception(result_.exception);
            break;
        case State::AsyncRetAbort:
            result = ScriptResult::abort(result_.abort.reason, result_.abort.position);
            break;
        default:
            return handle_error(ScriptResult::abort(ScriptAbortReason::InvalidState), epc);
    }

    async_.reset();
    state_ = State::Running;
    return apply_call_result(result, op, argc, epc);
}

} // namespace fiber::script::run
