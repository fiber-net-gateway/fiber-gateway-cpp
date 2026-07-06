#include "InterpreterVm.h"

#include <new>
#include <string>
#include <utility>

#include "../../common/Assert.h"
#include "../../common/json/JsGc.h"
#include "../Library.h"
#include "../Runtime.h"
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

ScriptResult status_to_result(ScriptStatus status, const fiber::json::JsValue &value) {
    if (status.is_success()) {
        return ScriptResult::success(value);
    }
    if (status.is_exception()) {
        return ScriptResult::exception(value);
    }
    return ScriptResult::abort(status.abort().reason, status.abort().position);
}

std::size_t estimate_iterator_next_bytes(const fiber::json::GcIterator *iter) {
    if (!iter) {
        return 0;
    }
    std::size_t bytes = 0;
    if (iter->kind == fiber::json::GcIteratorKind::Array) {
        if (iter->mode == fiber::json::GcIteratorMode::Entries) {
            bytes += fiber::json::gc_estimate_array_bytes(2);
        }
        return bytes;
    }
    if (iter->mode == fiber::json::GcIteratorMode::Entries) {
        bytes += fiber::json::gc_estimate_array_bytes(2);
    }
    if (!iter->using_snapshot && iter->object && iter->expected_version != iter->object->version) {
        bytes += fiber::json::gc_estimate_object_snapshot_bytes(iter->object->size);
    }
    return bytes;
}

std::size_t decode_call_site_index(std::int32_t instr) {
    return static_cast<std::size_t>(static_cast<std::uint32_t>(instr) >> 8);
}

bool opcode_uses_spread(std::uint8_t op) {
    return op == ir::Code::CALL_FUNC_SPREAD || op == ir::Code::CALL_ASYNC_FUNC_SPREAD;
}

Library::HostCallable::Kind expected_host_kind(std::uint8_t op) {
    switch (op) {
        case ir::Code::CALL_FUNC:
        case ir::Code::CALL_FUNC_SPREAD:
            return Library::HostCallable::Kind::SyncFunction;
        case ir::Code::CALL_ASYNC_FUNC:
        case ir::Code::CALL_ASYNC_FUNC_SPREAD:
            return Library::HostCallable::Kind::AsyncFunction;
        case ir::Code::CALL_CONST:
            return Library::HostCallable::Kind::SyncConstant;
        case ir::Code::CALL_ASYNC_CONST:
            return Library::HostCallable::Kind::AsyncConstant;
        default:
            FIBER_PANIC("invalid host call opcode");
    }
}

} // namespace

InterpreterVm::InterpreterVm(const ir::Compiled &compiled, const fiber::json::JsValue &root, void *attach,
                             ScriptRuntime &runtime) :
    compile_(compiled), root_(root), attach_(attach), runtime_(runtime) {
    FIBER_ASSERT(compile_.validate_operands());
    std::size_t total = compile_.stack_size + compile_.var_table_size;
    if (total > 0) {
        slots_.reset(new (std::nothrow) fiber::json::JsValue[total]);
        if (!slots_) {
            state_ = State::Abort;
            result_.abort = ScriptAbort{ScriptAbortReason::OutOfMemory, -1};
            runtime_.add_root_source(this);
            return;
        }
        stack_ = slots_.get();
        vars_ = stack_ + compile_.stack_size;
    }
    runtime_.add_root_source(this);
}

InterpreterVm::~InterpreterVm() { runtime_.remove_root_source(this); }

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
    if (async_.task.valid() && !async_.ready) {
        state_ = State::Suspend;
        return;
    }
    if (async_.task.valid() && async_.ready && !apply_async_ready()) {
        return;
    }
    state_ = State::Running;
    using BinaryOp = CallResult (*)(ScriptRuntime &, ConstValueHandle, ConstValueHandle, ResultPayload &) noexcept;
    auto apply_binary = [&](BinaryOp op, std::size_t epc) {
        FIBER_ASSERT(sp_ >= 2);
        fiber::json::JsValue *lhs = &stack_[sp_ - 2];
        fiber::json::JsValue *rhs = &stack_[sp_ - 1];
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
    using UnaryOp = CallResult (*)(ScriptRuntime &, ConstValueHandle, ResultPayload &) noexcept;
    auto apply_unary = [&](UnaryOp op, std::size_t epc) {
        FIBER_ASSERT(sp_ >= 1);
        fiber::json::JsValue *value = &stack_[sp_ - 1];
        CallResult status = op(runtime_, value, result_);
        if (!handle_call_result(status, epc)) {
            return false;
        }
        if (status == CallResult::Success) {
            *value = result_.value;
        }
        return true;
    };
    const auto &codes = compile_.codes;
    while (pc_ < codes.size()) {
        if (async_.task.valid() && async_.ready && !apply_async_ready()) {
            return;
        }
        std::int32_t instr = codes[pc_++];
        std::uint8_t op = static_cast<std::uint8_t>(instr & 0xFF);
        switch (op) {
            case ir::Code::NOOP:
                break;
            case ir::Code::LOAD_CONST: {
                std::size_t idx = static_cast<std::size_t>(instr >> 8);
                stack_[sp_++] = load_const(idx);
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
                stack_[sp_++] = vars_[static_cast<std::size_t>(instr >> 8)];
                break;
            case ir::Code::STORE_VAR:
                vars_[static_cast<std::size_t>(instr >> 8)] = stack_[--sp_];
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
                fiber::json::JsValue obj = fiber::json::JsValue::make_undefined();
                runtime_.run_with_gc_retry(fiber::json::gc_estimate_object_bytes(0), [&]() {
                    obj = fiber::json::JsValue::make_object(runtime_.heap(), 0);
                    return fiber::json::js_value_type(obj) == fiber::json::JsNodeType::Object;
                });
                if (fiber::json::js_value_type(obj) != fiber::json::JsNodeType::Object) {
                    ScriptResult error = make_oom(compile_.positions[pc_ - 1]);
                    if (!handle_error(error, pc_ - 1)) {
                        return;
                    }
                    continue;
                }
                stack_[sp_++] = obj;
                break;
            }
            case ir::Code::NEW_ARRAY: {
                fiber::json::JsValue arr = fiber::json::JsValue::make_undefined();
                runtime_.run_with_gc_retry(fiber::json::gc_estimate_array_bytes(0), [&]() {
                    arr = fiber::json::JsValue::make_array(runtime_.heap(), 0);
                    return fiber::json::js_value_type(arr) == fiber::json::JsNodeType::Array;
                });
                if (fiber::json::js_value_type(arr) != fiber::json::JsNodeType::Array) {
                    ScriptResult error = make_oom(compile_.positions[pc_ - 1]);
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
                fiber::json::JsValue *parent = &stack_[sp_ - 3];
                fiber::json::JsValue *key = &stack_[sp_ - 2];
                fiber::json::JsValue *value = &stack_[sp_ - 1];
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
                fiber::json::JsValue *parent = &stack_[sp_ - 3];
                fiber::json::JsValue *key = &stack_[sp_ - 2];
                fiber::json::JsValue *value = &stack_[sp_ - 1];
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
                std::size_t idx = static_cast<std::size_t>(instr >> 8);
                const auto *name = compile_.operand_string(idx);
                FIBER_ASSERT(name);
                ScriptRuntime::LocalMark mark(runtime_);
                ValueHandle key = runtime_.local_value();
                if (!key) {
                    ScriptResult error = make_oom(compile_.positions[pc_ - 1]);
                    if (!handle_error(error, pc_ - 1)) {
                        return;
                    }
                    continue;
                }
                *key = fiber::json::JsValue::make_native_string(const_cast<char *>(name->data()), name->size());
                fiber::json::JsValue *parent = &stack_[sp_ - 1];
                CallResult status = Access::prop_get(runtime_, parent, key, result_);
                if (!handle_call_result(status, pc_ - 1)) {
                    return;
                }
                parent[0] = result_.value;
                break;
            }
            case ir::Code::PROP_SET: {
                std::size_t idx = static_cast<std::size_t>(instr >> 8);
                const auto *name = compile_.operand_string(idx);
                FIBER_ASSERT(name);
                ScriptRuntime::LocalMark mark(runtime_);
                ValueHandle key = runtime_.local_value();
                if (!key) {
                    ScriptResult error = make_oom(compile_.positions[pc_ - 1]);
                    if (!handle_error(error, pc_ - 1)) {
                        return;
                    }
                    continue;
                }
                *key = fiber::json::JsValue::make_native_string(const_cast<char *>(name->data()), name->size());
                FIBER_ASSERT(sp_ >= 2);
                fiber::json::JsValue *parent = &stack_[sp_ - 2];
                fiber::json::JsValue *value = &stack_[sp_ - 1];
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
                std::size_t idx = static_cast<std::size_t>(instr >> 8);
                const auto *name = compile_.operand_string(idx);
                FIBER_ASSERT(name);
                ScriptRuntime::LocalMark mark(runtime_);
                ValueHandle key = runtime_.local_value();
                if (!key) {
                    ScriptResult error = make_oom(compile_.positions[pc_ - 1]);
                    if (!handle_error(error, pc_ - 1)) {
                        return;
                    }
                    continue;
                }
                *key = fiber::json::JsValue::make_native_string(const_cast<char *>(name->data()), name->size());
                FIBER_ASSERT(sp_ >= 2);
                fiber::json::JsValue *parent = &stack_[sp_ - 2];
                fiber::json::JsValue *value = &stack_[sp_ - 1];
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
                const auto &site = compile_.call_site_at(decode_call_site_index(instr));
                const bool is_spread = opcode_uses_spread(op);
                const AsyncResumeKind resume_kind =
                        is_spread ? AsyncResumeKind::ReplaceTop : AsyncResumeKind::PushResult;
                const auto &symbol = compile_.host_symbol_at(site.host_symbol_index);
                FIBER_ASSERT(symbol.kind == expected_host_kind(op));
                FIBER_ASSERT(is_spread == ((site.flags & ir::Compiled::CallSiteSpreadArgs) != 0));
                if (!dispatch_call_site(site, resume_kind)) {
                    return;
                }
                break;
            }
            case ir::Code::JUMP:
                pc_ = static_cast<std::size_t>(instr >> 8);
                break;
            case ir::Code::JUMP_IF_FALSE: {
                fiber::json::JsValue *cond = &stack_[sp_ - 1];
                if (!Compares::logic(cond)) {
                    pc_ = static_cast<std::size_t>(instr >> 8);
                }
                --sp_;
                break;
            }
            case ir::Code::JUMP_IF_TRUE: {
                fiber::json::JsValue *cond = &stack_[sp_ - 1];
                if (Compares::logic(cond)) {
                    pc_ = static_cast<std::size_t>(instr >> 8);
                }
                --sp_;
                break;
            }
            case ir::Code::ITERATE_INTO: {
                std::size_t idx = static_cast<std::size_t>(instr >> kInstrumentLen);
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
                std::size_t idx = static_cast<std::size_t>(instr >> kInstrumentLen);
                auto *iter = fiber::json::js_value_heap_ptr<fiber::json::GcIterator>(vars_[idx]);
                fiber::json::JsValue out;
                bool done = true;
                bool ok = runtime_.run_with_gc_retry(estimate_iterator_next_bytes(iter), [&]() {
                    return fiber::json::gc_iterator_next(&runtime_.heap(), iter, out, done);
                });
                stack_[sp_++] = fiber::json::JsValue::make_boolean(ok && !done);
                break;
            }
            case ir::Code::ITERATE_KEY: {
                std::size_t var_idx = static_cast<std::size_t>((instr >> kInstrumentLen) & kMaxIteratorVar);
                std::size_t iter_idx = static_cast<std::size_t>(instr >> kIteratorOff);
                auto *iter = fiber::json::js_value_heap_ptr<fiber::json::GcIterator>(vars_[iter_idx]);
                if (iter && iter->has_current) {
                    vars_[var_idx] = iter->current_key;
                } else {
                    vars_[var_idx] = fiber::json::JsValue::make_undefined();
                }
                break;
            }
            case ir::Code::ITERATE_VALUE: {
                std::size_t var_idx = static_cast<std::size_t>((instr >> kInstrumentLen) & kMaxIteratorVar);
                std::size_t iter_idx = static_cast<std::size_t>(instr >> kIteratorOff);
                auto *iter = fiber::json::js_value_heap_ptr<fiber::json::GcIterator>(vars_[iter_idx]);
                if (iter && iter->has_current) {
                    vars_[var_idx] = iter->current_value;
                } else {
                    vars_[var_idx] = fiber::json::JsValue::make_undefined();
                }
                break;
            }
            case ir::Code::INTO_CATCH: {
                std::size_t idx = static_cast<std::size_t>(instr >> kInstrumentLen);
                vars_[idx] = result_.exception;
                result_.exception = fiber::json::JsValue::make_undefined();
                break;
            }
            case ir::Code::END_RETURN: {
                if (sp_ > 0) {
                    result_.value = stack_[sp_ - 1];
                } else {
                    result_.value = fiber::json::JsValue::make_undefined();
                }
                state_ = State::Success;
                return;
            }
            case ir::Code::THROW_EXP: {
                fiber::json::JsValue thrown = stack_[--sp_];
                ScriptResult error = ScriptResult::exception(thrown);
                if (!handle_error(error, pc_ - 1)) {
                    return;
                }
                break;
            }
            default: {
                ScriptResult error = make_abort(ScriptAbortReason::InvalidOpcode, compile_.positions[pc_ - 1]);
                if (!handle_error(error, pc_ - 1)) {
                    return;
                }
                break;
            }
        }
    }
    result_.value = fiber::json::JsValue::make_undefined();
    state_ = State::Success;
}

void InterpreterVm::async_complete(void *context, ScriptStatus status) noexcept {
    auto *vm = static_cast<InterpreterVm *>(context);
    if (!vm || vm->done()) {
        return;
    }
    if (!vm->async_.task.valid() || vm->async_.ready) {
        return;
    }
    vm->async_.status = status;
    vm->async_.ready = true;
}

void InterpreterVm::visit_roots(fiber::json::GcRootVisitor &visitor) noexcept {
    visitor.visit(&root_);
    visitor.visit_range(stack_, sp_);
    visitor.visit_range(vars_, compile_.var_table_size);
    if (state_ == State::Running || state_ == State::Suspend || state_ == State::Success) {
        visitor.visit(&result_.value);
    }
    if (state_ == State::Exception) {
        visitor.visit(&result_.exception);
    }
    if (async_.ready || async_.task.valid()) {
        visitor.visit(&async_.value);
    }
    if (!async_.args.empty()) {
        visitor.visit_range(async_.args.data(), async_.args.size());
    }
}

fiber::json::JsValue *InterpreterVm::prepare_call_args(std::size_t off, std::size_t count) {
    (void) count;
    if (!stack_ || off >= compile_.stack_size) {
        return nullptr;
    }
    return stack_ + off;
}

fiber::json::JsValue *InterpreterVm::prepare_spread_call_args(std::size_t slot, std::uint32_t &argc) {
    async_.args.clear();
    argc = 0;
    if (!stack_ || slot >= compile_.stack_size) {
        return nullptr;
    }
    const fiber::json::JsValue &args = stack_[slot];
    if (fiber::json::js_value_type(args) != fiber::json::JsNodeType::Array) {
        return nullptr;
    }
    auto *arr = fiber::json::js_value_heap_ptr<const fiber::json::GcArray>(args);
    if (!arr || arr->size == 0) {
        return nullptr;
    }
    async_.args.reserve(arr->size);
    for (std::size_t i = 0; i < arr->size; ++i) {
        const fiber::json::JsValue *value = fiber::json::gc_array_get(arr, i);
        async_.args.push_back(value ? *value : fiber::json::JsValue::make_undefined());
    }
    argc = static_cast<std::uint32_t>(async_.args.size());
    return async_.args.data();
}

Library::HostCallFrame InterpreterVm::make_call_frame() const {
    Library::HostCallFrame frame;
    frame.runtime = const_cast<ScriptRuntime *>(&runtime_);
    frame.root = const_cast<fiber::json::JsValue *>(&root_);
    frame.attach = attach_;
    return frame;
}

bool InterpreterVm::dispatch_call_site(const ir::Compiled::CallSite &site, AsyncResumeKind resume_kind) {
    const auto &symbol = compile_.host_symbol_at(site.host_symbol_index);
    const bool is_spread = (site.flags & ir::Compiled::CallSiteSpreadArgs) != 0;
    if (!is_spread) {
        async_.args.clear();
    }
    std::uint32_t argc = site.argc;
    fiber::json::JsValue *args = nullptr;
    std::size_t arg_base = sp_;
    if (is_spread) {
        args = prepare_spread_call_args(sp_ - 1, argc);
    } else if (argc > 0) {
        FIBER_ASSERT(sp_ >= argc);
        arg_base = sp_ - argc;
        args = prepare_call_args(arg_base, argc);
    }
    const Library::HostCallFrame frame = make_call_frame();
    const Library::Arguments arguments{args, argc};
    const std::size_t epc = pc_ - 1;
    switch (symbol.kind) {
        case Library::HostCallable::Kind::SyncFunction: {
            FIBER_ASSERT(symbol.callable->function);
            ScriptRuntime::LocalMark mark(runtime_);
            ValueHandle out = runtime_.local_value();
            if (!out) {
                if (is_spread) {
                    async_.args.clear();
                }
                return handle_error(ScriptResult::abort(ScriptAbortReason::OutOfMemory), epc);
            }
            ScriptStatus status = symbol.callable->function(symbol.callable->userdata, frame, arguments, out);
            fiber::json::JsValue value = *out;
            if (is_spread) {
                async_.args.clear();
            } else {
                sp_ = arg_base;
            }
            return apply_call_result(status, value, resume_kind, epc);
        }
        case Library::HostCallable::Kind::SyncConstant: {
            FIBER_ASSERT(symbol.callable->constant);
            ScriptRuntime::LocalMark mark(runtime_);
            ValueHandle out = runtime_.local_value();
            if (!out) {
                if (is_spread) {
                    async_.args.clear();
                }
                return handle_error(ScriptResult::abort(ScriptAbortReason::OutOfMemory), epc);
            }
            ScriptStatus status = symbol.callable->constant(symbol.callable->userdata, frame, out);
            fiber::json::JsValue value = *out;
            if (is_spread) {
                async_.args.clear();
            }
            return apply_call_result(status, value, resume_kind, epc);
        }
        case Library::HostCallable::Kind::AsyncConstant: {
            FIBER_ASSERT(symbol.callable->async_constant);
            async_.ready = false;
            async_.resume_kind = resume_kind;
            async_.resume_epc = epc;
            async_.status = ScriptStatus::abort(ScriptAbortReason::InvalidState);
            async_.value = fiber::json::JsValue::make_undefined();
            async_.task = symbol.callable->async_constant(symbol.callable->userdata, frame, &async_.value);
            if (is_spread) {
                async_.args.clear();
            }
            if (!async_.task.valid()) {
                return handle_error(ScriptResult::abort(async_.task.allocation_failed()
                                                                ? ScriptAbortReason::OutOfMemory
                                                                : ScriptAbortReason::InvalidState),
                                    epc);
            }
            async_.task.set_completion({&InterpreterVm::async_complete, this});
            state_ = State::Suspend;
            return false;
        }
        case Library::HostCallable::Kind::AsyncFunction: {
            FIBER_ASSERT(symbol.callable->async_function);
            async_.ready = false;
            async_.resume_kind = resume_kind;
            async_.resume_epc = epc;
            async_.status = ScriptStatus::abort(ScriptAbortReason::InvalidState);
            async_.value = fiber::json::JsValue::make_undefined();
            if (!is_spread && argc > 0) {
                async_.args.clear();
                async_.args.reserve(argc);
                for (std::uint32_t i = 0; i < argc; ++i) {
                    async_.args.push_back(args ? args[i] : fiber::json::JsValue::make_undefined());
                }
                args = async_.args.data();
            }
            async_.arguments = Library::Arguments{args, argc};
            async_.task =
                    symbol.callable->async_function(symbol.callable->userdata, frame, async_.arguments, &async_.value);
            if (!is_spread) {
                sp_ = arg_base;
            }
            if (!async_.task.valid()) {
                async_.args.clear();
                return handle_error(ScriptResult::abort(async_.task.allocation_failed()
                                                                ? ScriptAbortReason::OutOfMemory
                                                                : ScriptAbortReason::InvalidState),
                                    epc);
            }
            async_.task.set_completion({&InterpreterVm::async_complete, this});
            state_ = State::Suspend;
            return false;
        }
    }
    return true;
}

bool InterpreterVm::apply_call_result(ScriptStatus status, const fiber::json::JsValue &value,
                                      AsyncResumeKind resume_kind, std::size_t resume_epc) {
    if (status.is_success()) {
        switch (resume_kind) {
            case AsyncResumeKind::PushResult:
                if (sp_ < compile_.stack_size) {
                    stack_[sp_] = value;
                }
                ++sp_;
                break;
            case AsyncResumeKind::ReplaceTop:
                if (sp_ > 0 && sp_ - 1 < compile_.stack_size) {
                    stack_[sp_ - 1] = value;
                }
                break;
            case AsyncResumeKind::None:
                break;
        }
        return true;
    }
    return handle_error(status_to_result(status, value), resume_epc);
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
    int target = search_catch(epc);
    sp_ = 0;
    if (target < 0) {
        return false;
    }
    pc_ = static_cast<std::size_t>(target);
    state_ = State::Running;
    return true;
}

int InterpreterVm::search_catch(std::size_t epc) const {
    const auto &table = compile_.exception_table;
    for (std::size_t i = table.size(); i >= 3; i -= 3) {
        std::size_t base = i - 3;
        auto try_begin = static_cast<std::size_t>(table[base]);
        auto catch_begin = static_cast<std::size_t>(table[base + 1]);
        if (try_begin <= epc && epc < catch_begin) {
            return table[base + 1];
        }
    }
    return -1;
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
    if (error.is_abort() && error.abort().position < 0 && epc < compile_.positions.size()) {
        result_.abort = ScriptAbort{error.abort().reason, compile_.positions[epc]};
    } else {
        result_.abort = error.abort();
    }
    state_ = State::Abort;
    return false;
}

fiber::json::JsValue InterpreterVm::load_const(std::size_t operand_index) const noexcept {
    const auto *cv = compile_.operand_const(operand_index);
    FIBER_ASSERT(cv);
    switch (cv->kind) {
        case ir::Compiled::ConstValue::Kind::Undefined:
            return fiber::json::JsValue::make_undefined();
        case ir::Compiled::ConstValue::Kind::Null:
            return fiber::json::JsValue::make_null();
        case ir::Compiled::ConstValue::Kind::Boolean:
            return fiber::json::JsValue::make_boolean(cv->bool_value);
        case ir::Compiled::ConstValue::Kind::Integer:
            return fiber::json::JsValue::make_integer(cv->int_value);
        case ir::Compiled::ConstValue::Kind::Float:
            return fiber::json::JsValue::make_float(cv->float_value);
        case ir::Compiled::ConstValue::Kind::String:
            return fiber::json::JsValue::make_native_string(cv->text.data(), cv->text.size());
        case ir::Compiled::ConstValue::Kind::Binary:
            return fiber::json::JsValue::make_native_binary(cv->bytes.data(), cv->bytes.size());
    }
    return fiber::json::JsValue::make_undefined();
}

bool InterpreterVm::apply_async_ready() {
    if (!async_.task.valid() || !async_.ready) {
        return true;
    }
    ScriptStatus status = async_.status;
    fiber::json::JsValue value = async_.value;
    AsyncResumeKind resume_kind = async_.resume_kind;
    std::size_t resume_epc = async_.resume_epc;
    async_.task.reset();
    async_.ready = false;
    async_.resume_kind = AsyncResumeKind::None;
    async_.resume_epc = 0;
    async_.status = ScriptStatus::abort(ScriptAbortReason::InvalidState);
    async_.value = fiber::json::JsValue::make_undefined();
    async_.args.clear();
    async_.arguments = Library::Arguments{};
    state_ = State::Running;
    return apply_call_result(status, value, resume_kind, resume_epc);
}

} // namespace fiber::script::run
