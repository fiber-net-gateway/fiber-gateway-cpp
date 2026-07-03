#include "InterpreterVm.h"

#include <map>
#include <set>
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
    compiled_(compiled), root_(root), attach_(attach), runtime_(runtime) {
    FIBER_ASSERT(compiled_.validate_operands());
    stack_size_ = compiled_.stack_size;
    var_count_ = compiled_.var_table_size;
    std::size_t total = stack_size_ + var_count_;
    slots_.resize(total);
    if (total > 0) {
        stack_ = slots_.data();
        vars_ = stack_ + stack_size_;
    }
    const_cache_.resize(compiled_.operands.size());
    const_cache_valid_.resize(compiled_.operands.size(), false);
    build_exception_index();
    runtime_.add_root_source(this);
}

InterpreterVm::~InterpreterVm() { runtime_.remove_root_source(this); }

void InterpreterVm::iterate() {
    if (done()) {
        return;
    }
    if (async_task_.valid() && !async_ready_) {
        return;
    }
    if (async_task_.valid() && async_ready_ && !apply_async_ready()) {
        return;
    }
    auto handle_status = [&](ScriptStatus status, fiber::json::JsValue *value, std::size_t epc) {
        if (status) {
            return true;
        }
        return handle_error(status_to_result(status, value ? *value : undefined_), epc);
    };
    using BinaryOp = ScriptStatus (*)(ScriptRuntime &, fiber::json::JsValue *, fiber::json::JsValue *,
                                      fiber::json::JsValue *) noexcept;
    auto apply_binary = [&](BinaryOp op, std::size_t epc) {
        FIBER_ASSERT(sp_ >= 2);
        fiber::json::JsValue *lhs = &stack_[sp_ - 2];
        fiber::json::JsValue *rhs = &stack_[sp_ - 1];
        ScriptStatus status = op(runtime_, lhs, lhs, rhs);
        if (!handle_status(status, lhs, epc)) {
            return false;
        }
        if (status) {
            --sp_;
        }
        return true;
    };
    using UnaryOp = ScriptStatus (*)(fiber::json::JsValue *, fiber::json::JsValue *) noexcept;
    auto apply_unary = [&](UnaryOp op, std::size_t epc) {
        FIBER_ASSERT(sp_ >= 1);
        fiber::json::JsValue *value = &stack_[sp_ - 1];
        ScriptStatus status = op(value, value);
        if (!handle_status(status, value, epc)) {
            return false;
        }
        return true;
    };
    using RuntimeUnaryOp = ScriptStatus (*)(ScriptRuntime &, fiber::json::JsValue *, fiber::json::JsValue *) noexcept;
    auto apply_runtime_unary = [&](RuntimeUnaryOp op, std::size_t epc) {
        FIBER_ASSERT(sp_ >= 1);
        fiber::json::JsValue *value = &stack_[sp_ - 1];
        ScriptStatus status = op(runtime_, value, value);
        if (!handle_status(status, value, epc)) {
            return false;
        }
        return true;
    };
    using AccessBinaryOp = ScriptStatus (*)(ScriptRuntime &, fiber::json::JsValue *, fiber::json::JsValue *,
                                            fiber::json::JsValue *) noexcept;
    auto apply_access_binary = [&](AccessBinaryOp op, std::size_t epc) {
        FIBER_ASSERT(sp_ >= 2);
        fiber::json::JsValue *target = &stack_[sp_ - 2];
        fiber::json::JsValue *addition = &stack_[sp_ - 1];
        ScriptStatus status = op(runtime_, target, target, addition);
        if (!handle_status(status, target, epc)) {
            return false;
        }
        if (status) {
            --sp_;
        }
        return true;
    };
    const auto &codes = compiled_.codes;
    while (pc_ < codes.size()) {
        if (async_task_.valid() && async_ready_ && !apply_async_ready()) {
            return;
        }
        std::int32_t instr = codes[pc_++];
        std::uint8_t op = static_cast<std::uint8_t>(instr & 0xFF);
        switch (op) {
            case ir::Code::NOOP:
                break;
            case ir::Code::LOAD_CONST: {
                std::size_t idx = static_cast<std::size_t>(instr >> 8);
                ScriptResult loaded = load_const(idx);
                if (!loaded) {
                    if (!handle_error(loaded, pc_ - 1)) {
                        return;
                    }
                    continue;
                }
                stack_[sp_++] = loaded.value();
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
                if (!apply_runtime_unary(&Unaries::typeof_op, pc_ - 1)) {
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
                    ScriptResult error = make_oom(compiled_.positions[pc_ - 1]);
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
                    ScriptResult error = make_oom(compiled_.positions[pc_ - 1]);
                    if (!handle_error(error, pc_ - 1)) {
                        return;
                    }
                    continue;
                }
                stack_[sp_++] = arr;
                break;
            }
            case ir::Code::EXP_OBJECT:
                if (!apply_access_binary(&Access::expand_object, pc_ - 1)) {
                    return;
                }
                break;
            case ir::Code::EXP_ARRAY:
                if (!apply_access_binary(&Access::expand_array, pc_ - 1)) {
                    return;
                }
                break;
            case ir::Code::PUSH_ARRAY:
                if (!apply_access_binary(&Access::push_array, pc_ - 1)) {
                    return;
                }
                break;
            case ir::Code::IDX_GET:
                if (!apply_access_binary(&Access::index_get, pc_ - 1)) {
                    return;
                }
                break;
            case ir::Code::IDX_SET: {
                FIBER_ASSERT(sp_ >= 3);
                fiber::json::JsValue *parent = &stack_[sp_ - 3];
                fiber::json::JsValue *key = &stack_[sp_ - 2];
                fiber::json::JsValue *value = &stack_[sp_ - 1];
                ScriptStatus status = Access::index_set(runtime_, parent, parent, key, value);
                if (!handle_status(status, parent, pc_ - 1)) {
                    return;
                }
                if (status) {
                    sp_ -= 2;
                }
                break;
            }
            case ir::Code::IDX_SET_1: {
                FIBER_ASSERT(sp_ >= 3);
                fiber::json::JsValue *parent = &stack_[sp_ - 3];
                fiber::json::JsValue *key = &stack_[sp_ - 2];
                fiber::json::JsValue *value = &stack_[sp_ - 1];
                ScriptStatus status = Access::index_set1(runtime_, parent, parent, key, value);
                if (!handle_status(status, parent, pc_ - 1)) {
                    return;
                }
                if (status) {
                    sp_ -= 2;
                }
                break;
            }
            case ir::Code::PROP_GET: {
                std::size_t idx = static_cast<std::size_t>(instr >> 8);
                const auto *name = compiled_.operand_string(idx);
                FIBER_ASSERT(name);
                ScriptRuntime::LocalMark mark(runtime_);
                ValueHandle key = runtime_.local_value();
                if (!key) {
                    ScriptResult error = make_oom(compiled_.positions[pc_ - 1]);
                    if (!handle_error(error, pc_ - 1)) {
                        return;
                    }
                    continue;
                }
                *key = fiber::json::JsValue::make_native_string(const_cast<char *>(name->data()), name->size());
                fiber::json::JsValue *parent = &stack_[sp_ - 1];
                ScriptStatus status = Access::prop_get(runtime_, parent, parent, key);
                if (!handle_status(status, parent, pc_ - 1)) {
                    return;
                }
                break;
            }
            case ir::Code::PROP_SET: {
                std::size_t idx = static_cast<std::size_t>(instr >> 8);
                const auto *name = compiled_.operand_string(idx);
                FIBER_ASSERT(name);
                ScriptRuntime::LocalMark mark(runtime_);
                ValueHandle key = runtime_.local_value();
                if (!key) {
                    ScriptResult error = make_oom(compiled_.positions[pc_ - 1]);
                    if (!handle_error(error, pc_ - 1)) {
                        return;
                    }
                    continue;
                }
                *key = fiber::json::JsValue::make_native_string(const_cast<char *>(name->data()), name->size());
                FIBER_ASSERT(sp_ >= 2);
                fiber::json::JsValue *parent = &stack_[sp_ - 2];
                fiber::json::JsValue *value = &stack_[sp_ - 1];
                ScriptStatus status = Access::prop_set(runtime_, parent, parent, value, key);
                if (!handle_status(status, parent, pc_ - 1)) {
                    return;
                }
                if (status) {
                    --sp_;
                }
                break;
            }
            case ir::Code::PROP_SET_1: {
                std::size_t idx = static_cast<std::size_t>(instr >> 8);
                const auto *name = compiled_.operand_string(idx);
                FIBER_ASSERT(name);
                ScriptRuntime::LocalMark mark(runtime_);
                ValueHandle key = runtime_.local_value();
                if (!key) {
                    ScriptResult error = make_oom(compiled_.positions[pc_ - 1]);
                    if (!handle_error(error, pc_ - 1)) {
                        return;
                    }
                    continue;
                }
                *key = fiber::json::JsValue::make_native_string(const_cast<char *>(name->data()), name->size());
                FIBER_ASSERT(sp_ >= 2);
                fiber::json::JsValue *parent = &stack_[sp_ - 2];
                fiber::json::JsValue *value = &stack_[sp_ - 1];
                ScriptStatus status = Access::prop_set1(runtime_, parent, parent, value, key);
                if (!handle_status(status, parent, pc_ - 1)) {
                    return;
                }
                if (status) {
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
                const auto &site = compiled_.call_site_at(decode_call_site_index(instr));
                const bool is_spread = opcode_uses_spread(op);
                const AsyncResumeKind resume_kind =
                        is_spread ? AsyncResumeKind::ReplaceTop : AsyncResumeKind::PushResult;
                const auto &symbol = compiled_.host_symbol_at(site.host_symbol_index);
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
                ScriptStatus status = Unaries::iterate(runtime_, &vars_[idx], &stack_[sp_ - 1]);
                if (!handle_status(status, &vars_[idx], pc_ - 1)) {
                    return;
                }
                if (status) {
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
                vars_[idx] = pending_value_;
                pending_value_kind_ = PendingValueKind::None;
                pending_value_ = fiber::json::JsValue::make_undefined();
                break;
            }
            case ir::Code::END_RETURN: {
                if (sp_ > 0) {
                    pending_value_ = stack_[sp_ - 1];
                } else {
                    pending_value_ = fiber::json::JsValue::make_undefined();
                }
                pending_value_kind_ = PendingValueKind::Return;
                result_ = ScriptResult::success(pending_value_);
                return;
            }
            case ir::Code::THROW_EXP: {
                fiber::json::JsValue thrown = stack_[--sp_];
                pending_value_ = thrown;
                pending_value_kind_ = PendingValueKind::Thrown;
                ScriptResult error = ScriptResult::exception(thrown);
                if (!handle_error(error, pc_ - 1)) {
                    return;
                }
                break;
            }
            default: {
                ScriptResult error = make_abort(ScriptAbortReason::InvalidOpcode, compiled_.positions[pc_ - 1]);
                if (!handle_error(error, pc_ - 1)) {
                    return;
                }
                break;
            }
        }
    }
    handle_error(make_abort(ScriptAbortReason::NoReturn, -1), pc_ == 0 ? 0 : pc_ - 1);
}

void InterpreterVm::async_complete(void *context, ScriptStatus status) noexcept {
    auto *vm = static_cast<InterpreterVm *>(context);
    if (!vm || vm->done()) {
        return;
    }
    if (!vm->async_task_.valid() || vm->async_ready_) {
        return;
    }
    vm->async_status_ = status;
    vm->async_ready_ = true;
}

void InterpreterVm::visit_roots(fiber::json::GcRootVisitor &visitor) noexcept {
    visitor.visit(&root_);
    visitor.visit_range(stack_, sp_);
    visitor.visit_range(vars_, var_count_);
    for (std::size_t i = 0; i < const_cache_.size(); ++i) {
        if (const_cache_valid_[i]) {
            visitor.visit(&const_cache_[i]);
        }
    }
    if (pending_value_kind_ == PendingValueKind::Thrown) {
        visitor.visit(&pending_value_);
    }
    if (async_ready_) {
        visitor.visit(&async_value_);
    } else if (async_task_.valid()) {
        visitor.visit(&async_value_);
    }
    if (pending_value_kind_ == PendingValueKind::Return) {
        visitor.visit(&pending_value_);
    }
    if (!call_args_.empty()) {
        visitor.visit_range(call_args_.data(), call_args_.size());
    }
}

fiber::json::JsValue *InterpreterVm::prepare_call_args(std::size_t off, std::size_t count) {
    if (!stack_ || off >= stack_size_) {
        return &undefined_;
    }
    return stack_ + off;
}

fiber::json::JsValue *InterpreterVm::prepare_spread_call_args(std::size_t slot, std::uint32_t &argc) {
    call_args_.clear();
    argc = 0;
    if (!stack_ || slot >= stack_size_) {
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
    call_args_.reserve(arr->size);
    for (std::size_t i = 0; i < arr->size; ++i) {
        const fiber::json::JsValue *value = fiber::json::gc_array_get(arr, i);
        call_args_.push_back(value ? *value : fiber::json::JsValue::make_undefined());
    }
    argc = static_cast<std::uint32_t>(call_args_.size());
    return call_args_.data();
}

Library::HostCallFrame InterpreterVm::make_call_frame() const {
    Library::HostCallFrame frame;
    frame.runtime = const_cast<ScriptRuntime *>(&runtime_);
    frame.root = const_cast<fiber::json::JsValue *>(&root_);
    frame.attach = attach_;
    return frame;
}

bool InterpreterVm::dispatch_call_site(const ir::Compiled::CallSite &site, AsyncResumeKind resume_kind) {
    const auto &symbol = compiled_.host_symbol_at(site.host_symbol_index);
    const bool is_spread = (site.flags & ir::Compiled::CallSiteSpreadArgs) != 0;
    if (!is_spread) {
        call_args_.clear();
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
                    call_args_.clear();
                }
                return handle_error(ScriptResult::abort(ScriptAbortReason::OutOfMemory), epc);
            }
            ScriptStatus status = symbol.callable->function(symbol.callable->userdata, frame, arguments, out);
            fiber::json::JsValue value = *out;
            if (is_spread) {
                call_args_.clear();
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
                    call_args_.clear();
                }
                return handle_error(ScriptResult::abort(ScriptAbortReason::OutOfMemory), epc);
            }
            ScriptStatus status = symbol.callable->constant(symbol.callable->userdata, frame, out);
            fiber::json::JsValue value = *out;
            if (is_spread) {
                call_args_.clear();
            }
            return apply_call_result(status, value, resume_kind, epc);
        }
        case Library::HostCallable::Kind::AsyncConstant: {
            FIBER_ASSERT(symbol.callable->async_constant);
            async_ready_ = false;
            async_resume_kind_ = resume_kind;
            async_resume_epc_ = epc;
            async_status_ = ScriptStatus::abort(ScriptAbortReason::InvalidState);
            async_value_ = fiber::json::JsValue::make_undefined();
            async_task_ = symbol.callable->async_constant(symbol.callable->userdata, frame, &async_value_);
            if (is_spread) {
                call_args_.clear();
            }
            if (!async_task_.valid()) {
                return handle_error(ScriptResult::abort(async_task_.allocation_failed()
                                                                ? ScriptAbortReason::OutOfMemory
                                                                : ScriptAbortReason::InvalidState),
                                    epc);
            }
            async_task_.set_completion({&InterpreterVm::async_complete, this});
            return false;
        }
        case Library::HostCallable::Kind::AsyncFunction: {
            FIBER_ASSERT(symbol.callable->async_function);
            async_ready_ = false;
            async_resume_kind_ = resume_kind;
            async_resume_epc_ = epc;
            async_status_ = ScriptStatus::abort(ScriptAbortReason::InvalidState);
            async_value_ = fiber::json::JsValue::make_undefined();
            async_task_ = symbol.callable->async_function(symbol.callable->userdata, frame, arguments, &async_value_);
            if (is_spread) {
                call_args_.clear();
            } else {
                sp_ = arg_base;
            }
            if (!async_task_.valid()) {
                return handle_error(ScriptResult::abort(async_task_.allocation_failed()
                                                                ? ScriptAbortReason::OutOfMemory
                                                                : ScriptAbortReason::InvalidState),
                                    epc);
            }
            async_task_.set_completion({&InterpreterVm::async_complete, this});
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
                if (sp_ < stack_size_) {
                    stack_[sp_] = value;
                }
                ++sp_;
                break;
            case AsyncResumeKind::ReplaceTop:
                if (sp_ > 0 && sp_ - 1 < stack_size_) {
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

bool InterpreterVm::catch_for_exception(std::size_t epc) {
    int target = search_catch(epc);
    sp_ = 0;
    if (target < 0) {
        return false;
    }
    pc_ = static_cast<std::size_t>(target);
    return true;
}

int InterpreterVm::search_catch(std::size_t epc) const {
    if (exp_ins_.empty()) {
        return -1;
    }
    int len = static_cast<int>(exp_ins_.size() >> 1);
    if (len == 0) {
        return -1;
    }
    if (epc < static_cast<std::size_t>(exp_ins_[0]) || static_cast<std::size_t>(exp_ins_[len - 1]) <= epc) {
        return -1;
    }
    if (len <= 8) {
        for (int i = 1; i < len; ++i) {
            if (static_cast<std::size_t>(exp_ins_[i]) >= epc) {
                return exp_ins_[i - 1 + len];
            }
        }
        return exp_ins_[len - 1 + len];
    }
    int l = 0;
    int r = len;
    while (l < r) {
        int m = (l + r) >> 1;
        int mv = exp_ins_[m];
        if (epc < static_cast<std::size_t>(mv)) {
            r = m;
        } else if (epc > static_cast<std::size_t>(mv)) {
            l = m + 1;
        } else {
            return exp_ins_[m + len];
        }
    }
    return exp_ins_[l - 1 + len];
}

void InterpreterVm::build_exception_index() {
    exp_ins_.clear();
    if (compiled_.exception_table.empty()) {
        return;
    }
    std::map<int, int> ranges;
    std::set<int> catches;
    for (std::size_t i = 0; i + 2 < compiled_.exception_table.size(); i += 3) {
        int try_begin = compiled_.exception_table[i];
        int catch_begin = compiled_.exception_table[i + 1];
        int catch_end = compiled_.exception_table[i + 2];
        ranges[try_begin] = catch_begin;
        catches.insert(catch_begin);
        auto latter = catches.lower_bound(catch_end);
        if (latter != catches.end()) {
            ranges[catch_begin] = *latter;
        } else {
            ranges[catch_begin] = -1;
        }
    }
    std::size_t size = ranges.size();
    exp_ins_.resize(size * 2);
    std::size_t idx = 0;
    for (const auto &entry: ranges) {
        exp_ins_[idx] = entry.first;
        exp_ins_[idx + size] = entry.second;
        ++idx;
    }
}

bool InterpreterVm::handle_error(ScriptResult error, std::size_t epc) {
    if (error.is_exception()) {
        pending_value_ = error.exception();
        pending_value_kind_ = PendingValueKind::Thrown;
        if (catch_for_exception(epc)) {
            return true;
        }
        result_ = error;
        return false;
    }
    if (error.is_abort() && error.abort().position < 0 && epc < compiled_.positions.size()) {
        result_ = ScriptResult::abort(error.abort().reason, compiled_.positions[epc]);
    } else {
        result_ = error;
    }
    return false;
}

ScriptResult InterpreterVm::load_const(std::size_t operand_index) {
    if (const_cache_valid_[operand_index]) {
        return const_cache_[operand_index];
    }
    const auto *cv = compiled_.operand_const(operand_index);
    FIBER_ASSERT(cv);
    fiber::json::JsValue value = fiber::json::JsValue::make_undefined();
    switch (cv->kind) {
        case ir::Compiled::ConstValue::Kind::Undefined:
            value = fiber::json::JsValue::make_undefined();
            break;
        case ir::Compiled::ConstValue::Kind::Null:
            value = fiber::json::JsValue::make_null();
            break;
        case ir::Compiled::ConstValue::Kind::Boolean:
            value = fiber::json::JsValue::make_boolean(cv->bool_value);
            break;
        case ir::Compiled::ConstValue::Kind::Integer:
            value = fiber::json::JsValue::make_integer(cv->int_value);
            break;
        case ir::Compiled::ConstValue::Kind::Float:
            value = fiber::json::JsValue::make_float(cv->float_value);
            break;
        case ir::Compiled::ConstValue::Kind::String: {
            runtime_.run_with_gc_retry(fiber::json::gc_estimate_utf8_string_bytes(cv->text.size()), [&]() {
                value = fiber::json::JsValue::make_string(runtime_.heap(), cv->text.data(), cv->text.size());
                return fiber::json::js_value_type(value) == fiber::json::JsNodeType::String &&
                       !fiber::json::js_value_is_borrowed_string(value);
            });
            if (fiber::json::js_value_type(value) != fiber::json::JsNodeType::String ||
                fiber::json::js_value_is_borrowed_string(value)) {
                return make_oom(-1);
            }
            break;
        }
        case ir::Compiled::ConstValue::Kind::Binary: {
            runtime_.run_with_gc_retry(fiber::json::gc_estimate_binary_bytes(cv->bytes.size()), [&]() {
                value = fiber::json::JsValue::make_binary(runtime_.heap(), cv->bytes.data(), cv->bytes.size());
                return fiber::json::js_value_type(value) == fiber::json::JsNodeType::Binary &&
                       !fiber::json::js_value_is_borrowed_binary(value);
            });
            if (fiber::json::js_value_type(value) != fiber::json::JsNodeType::Binary ||
                fiber::json::js_value_is_borrowed_binary(value)) {
                return make_oom(-1);
            }
            break;
        }
    }
    const_cache_[operand_index] = value;
    const_cache_valid_[operand_index] = true;
    return value;
}

bool InterpreterVm::apply_async_ready() {
    if (!async_task_.valid() || !async_ready_) {
        return true;
    }
    ScriptStatus status = async_status_;
    fiber::json::JsValue value = async_value_;
    AsyncResumeKind resume_kind = async_resume_kind_;
    std::size_t resume_epc = async_resume_epc_;
    async_task_.reset();
    async_ready_ = false;
    async_resume_kind_ = AsyncResumeKind::None;
    async_resume_epc_ = 0;
    async_status_ = ScriptStatus::abort(ScriptAbortReason::InvalidState);
    async_value_ = fiber::json::JsValue::make_undefined();
    return apply_call_result(status, value, resume_kind, resume_epc);
}

} // namespace fiber::script::run
