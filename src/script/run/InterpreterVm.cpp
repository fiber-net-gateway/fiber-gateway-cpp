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
    runtime_.roots().add_provider(this);
}

InterpreterVm::~InterpreterVm() { runtime_.roots().remove_provider(this); }

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
    auto finish_error = [&](ScriptResult error) { handle_error(error, pc_ == 0 ? 0 : pc_ - 1); };
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
                --sp_;
                {
                    ScriptResult result = Binaries::plus(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result, pc_ - 1)) {
                            return;
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::BOP_MINUS:
                --sp_;
                {
                    ScriptResult result = Binaries::minus(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result, pc_ - 1)) {
                            return;
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::BOP_MULTIPLY:
                --sp_;
                {
                    ScriptResult result = Binaries::multiply(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result, pc_ - 1)) {
                            return;
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::BOP_DIVIDE:
                --sp_;
                {
                    ScriptResult result = Binaries::divide(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result, pc_ - 1)) {
                            return;
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::BOP_MOD:
                --sp_;
                {
                    ScriptResult result = Binaries::modulo(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result, pc_ - 1)) {
                            return;
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::BOP_MATCH:
                --sp_;
                {
                    ScriptResult result = Binaries::matches(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result, pc_ - 1)) {
                            return;
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::BOP_LT:
                --sp_;
                {
                    ScriptResult result = Binaries::lt(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result, pc_ - 1)) {
                            return;
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::BOP_LTE:
                --sp_;
                {
                    ScriptResult result = Binaries::lte(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result, pc_ - 1)) {
                            return;
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::BOP_GT:
                --sp_;
                {
                    ScriptResult result = Binaries::gt(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result, pc_ - 1)) {
                            return;
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::BOP_GTE:
                --sp_;
                {
                    ScriptResult result = Binaries::gte(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result, pc_ - 1)) {
                            finish_error(result);
                            return;
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::BOP_EQ:
                --sp_;
                {
                    ScriptResult result = Binaries::eq(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result, pc_ - 1)) {
                            finish_error(result);
                            return;
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::BOP_SEQ:
                --sp_;
                {
                    ScriptResult result = Binaries::seq(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result, pc_ - 1)) {
                            finish_error(result);
                            return;
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::BOP_NE:
                --sp_;
                {
                    ScriptResult result = Binaries::ne(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result, pc_ - 1)) {
                            finish_error(result);
                            return;
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::BOP_SNE:
                --sp_;
                {
                    ScriptResult result = Binaries::sne(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result, pc_ - 1)) {
                            finish_error(result);
                            return;
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::BOP_IN:
                --sp_;
                {
                    ScriptResult result = Binaries::in(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result, pc_ - 1)) {
                            finish_error(result);
                            return;
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::UNARY_PLUS: {
                ScriptResult result = Unaries::plus(stack_[sp_ - 1]);
                if (!result) {
                    if (!handle_error(result, pc_ - 1)) {
                        finish_error(result);
                        return;
                    }
                    continue;
                }
                stack_[sp_ - 1] = result.value();
            } break;
            case ir::Code::UNARY_MINUS: {
                ScriptResult result = Unaries::minus(stack_[sp_ - 1]);
                if (!result) {
                    if (!handle_error(result, pc_ - 1)) {
                        finish_error(result);
                        return;
                    }
                    continue;
                }
                stack_[sp_ - 1] = result.value();
            } break;
            case ir::Code::UNARY_NEG: {
                ScriptResult result = Unaries::neg(stack_[sp_ - 1]);
                if (!result) {
                    if (!handle_error(result, pc_ - 1)) {
                        finish_error(result);
                        return;
                    }
                    continue;
                }
                stack_[sp_ - 1] = result.value();
            } break;
            case ir::Code::UNARY_TYPEOF: {
                ScriptResult result = Unaries::typeof_op(stack_[sp_ - 1], runtime_);
                if (!result) {
                    if (!handle_error(result, pc_ - 1)) {
                        finish_error(result);
                        return;
                    }
                    continue;
                }
                stack_[sp_ - 1] = result.value();
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
                        finish_error(error);
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
                        finish_error(error);
                        return;
                    }
                    continue;
                }
                stack_[sp_++] = arr;
                break;
            }
            case ir::Code::EXP_OBJECT:
                --sp_;
                {
                    ScriptResult result = Access::expand_object(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result, pc_ - 1)) {
                            finish_error(result);
                            return;
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::EXP_ARRAY:
                --sp_;
                {
                    ScriptResult result = Access::expand_array(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result, pc_ - 1)) {
                            finish_error(result);
                            return;
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::PUSH_ARRAY:
                --sp_;
                {
                    ScriptResult result = Access::push_array(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result, pc_ - 1)) {
                            finish_error(result);
                            return;
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::IDX_GET:
                --sp_;
                {
                    ScriptResult result = Access::index_get(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result, pc_ - 1)) {
                            finish_error(result);
                            return;
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::IDX_SET:
                sp_ -= 2;
                {
                    ScriptResult result = Access::index_set(stack_[sp_ - 1], stack_[sp_], stack_[sp_ + 1], runtime_);
                    if (!result) {
                        if (!handle_error(result, pc_ - 1)) {
                            finish_error(result);
                            return;
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::IDX_SET_1:
                sp_ -= 2;
                {
                    ScriptResult result = Access::index_set1(stack_[sp_ - 1], stack_[sp_], stack_[sp_ + 1], runtime_);
                    if (!result) {
                        if (!handle_error(result, pc_ - 1)) {
                            finish_error(result);
                            return;
                        }
                        continue;
                    }
                }
                break;
            case ir::Code::PROP_GET: {
                std::size_t idx = static_cast<std::size_t>(instr >> 8);
                const auto *name = compiled_.operand_string(idx);
                FIBER_ASSERT(name);
                fiber::json::JsValue key =
                        fiber::json::JsValue::make_native_string(const_cast<char *>(name->data()), name->size());
                ScriptResult result = Access::prop_get(stack_[sp_ - 1], key, runtime_);
                if (!result) {
                    if (!handle_error(result, pc_ - 1)) {
                        finish_error(result);
                        return;
                    }
                    continue;
                }
                stack_[sp_ - 1] = result.value();
                break;
            }
            case ir::Code::PROP_SET: {
                std::size_t idx = static_cast<std::size_t>(instr >> 8);
                const auto *name = compiled_.operand_string(idx);
                FIBER_ASSERT(name);
                fiber::json::JsValue key =
                        fiber::json::JsValue::make_native_string(const_cast<char *>(name->data()), name->size());
                --sp_;
                ScriptResult result = Access::prop_set(stack_[sp_ - 1], stack_[sp_], key, runtime_);
                if (!result) {
                    if (!handle_error(result, pc_ - 1)) {
                        finish_error(result);
                        return;
                    }
                    continue;
                }
                stack_[sp_ - 1] = result.value();
                break;
            }
            case ir::Code::PROP_SET_1: {
                std::size_t idx = static_cast<std::size_t>(instr >> 8);
                const auto *name = compiled_.operand_string(idx);
                FIBER_ASSERT(name);
                fiber::json::JsValue key =
                        fiber::json::JsValue::make_native_string(const_cast<char *>(name->data()), name->size());
                --sp_;
                ScriptResult result = Access::prop_set1(stack_[sp_ - 1], stack_[sp_], key, runtime_);
                if (!result) {
                    if (!handle_error(result, pc_ - 1)) {
                        finish_error(result);
                        return;
                    }
                    continue;
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
                if ((op == ir::Code::CALL_FUNC || op == ir::Code::CALL_ASYNC_FUNC) && !is_spread) {
                    sp_ -= site.argc;
                }
                if (!dispatch_call_site(site, resume_kind)) {
                    return;
                }
                break;
            }
            case ir::Code::JUMP:
                pc_ = static_cast<std::size_t>(instr >> 8);
                break;
            case ir::Code::JUMP_IF_FALSE: {
                fiber::json::JsValue cond = stack_[--sp_];
                if (!Compares::logic(cond)) {
                    pc_ = static_cast<std::size_t>(instr >> 8);
                }
                break;
            }
            case ir::Code::JUMP_IF_TRUE: {
                fiber::json::JsValue cond = stack_[--sp_];
                if (Compares::logic(cond)) {
                    pc_ = static_cast<std::size_t>(instr >> 8);
                }
                break;
            }
            case ir::Code::ITERATE_INTO: {
                std::size_t idx = static_cast<std::size_t>(instr >> kInstrumentLen);
                ScriptResult result = Unaries::iterate(stack_[--sp_], runtime_);
                if (!result) {
                    if (!handle_error(result, pc_ - 1)) {
                        finish_error(result);
                        return;
                    }
                    continue;
                }
                vars_[idx] = result.value();
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
                    finish_error(error);
                    return;
                }
                break;
            }
            default: {
                ScriptResult error = make_abort(ScriptAbortReason::InvalidOpcode, compiled_.positions[pc_ - 1]);
                if (!handle_error(error, pc_ - 1)) {
                    finish_error(error);
                    return;
                }
                break;
            }
        }
    }
    finish_error(make_abort(ScriptAbortReason::NoReturn, -1));
}

void InterpreterVm::async_complete(void *context, const ScriptResult &result) noexcept {
    auto *vm = static_cast<InterpreterVm *>(context);
    if (!vm || vm->done()) {
        return;
    }
    if (!vm->async_task_.valid() || vm->async_ready_) {
        return;
    }
    vm->async_result_ = result;
    vm->async_ready_ = true;
}

void InterpreterVm::visit_roots(fiber::json::GcRootSet::RootVisitor &visitor) {
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
        if (async_result_.is_success()) {
            visitor.visit(const_cast<fiber::json::JsValue *>(&async_result_.value()));
        } else if (async_result_.is_exception()) {
            visitor.visit(const_cast<fiber::json::JsValue *>(&async_result_.exception()));
        }
    }
    if (pending_value_kind_ == PendingValueKind::Return) {
        visitor.visit(&pending_value_);
    }
    if (!call_args_.empty()) {
        visitor.visit_range(call_args_.data(), call_args_.size());
    }
}

const fiber::json::JsValue *InterpreterVm::prepare_call_args(std::size_t off, std::size_t count) {
    if (!stack_ || off >= stack_size_) {
        return &undefined_;
    }
    return stack_ + off;
}

const fiber::json::JsValue *InterpreterVm::prepare_spread_call_args(std::size_t slot, std::uint32_t &argc) {
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
    frame.root = &root_;
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
    const fiber::json::JsValue *args = nullptr;
    if (is_spread) {
        args = prepare_spread_call_args(sp_ - 1, argc);
    } else if (argc > 0) {
        args = prepare_call_args(sp_, argc);
    }
    const Library::HostCallFrame frame = make_call_frame();
    const Library::Arguments arguments{args, argc};
    const std::size_t epc = pc_ - 1;
    switch (symbol.kind) {
        case Library::HostCallable::Kind::SyncFunction: {
            FIBER_ASSERT(symbol.callable->function);
            ScriptResult result = symbol.callable->function(symbol.callable->userdata, frame, arguments);
            if (is_spread) {
                call_args_.clear();
            }
            return apply_call_result(result, resume_kind, epc);
        }
        case Library::HostCallable::Kind::SyncConstant: {
            FIBER_ASSERT(symbol.callable->constant);
            ScriptResult result = symbol.callable->constant(symbol.callable->userdata, frame);
            if (is_spread) {
                call_args_.clear();
            }
            return apply_call_result(result, resume_kind, epc);
        }
        case Library::HostCallable::Kind::AsyncConstant: {
            FIBER_ASSERT(symbol.callable->async_constant);
            async_ready_ = false;
            async_resume_kind_ = resume_kind;
            async_resume_epc_ = epc;
            async_result_ = ScriptResult{};
            async_task_ = symbol.callable->async_constant(symbol.callable->userdata, frame);
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
            async_result_ = ScriptResult{};
            async_task_ = symbol.callable->async_function(symbol.callable->userdata, frame, arguments);
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
    }
    return true;
}

bool InterpreterVm::apply_call_result(const ScriptResult &result, AsyncResumeKind resume_kind, std::size_t resume_epc) {
    if (result.is_success()) {
        switch (resume_kind) {
            case AsyncResumeKind::PushResult:
                if (sp_ < stack_size_) {
                    stack_[sp_] = result.value();
                }
                ++sp_;
                break;
            case AsyncResumeKind::ReplaceTop:
                if (sp_ > 0 && sp_ - 1 < stack_size_) {
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case AsyncResumeKind::None:
                break;
        }
        return true;
    }
    return handle_error(result, resume_epc);
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
    ScriptResult result = async_result_;
    AsyncResumeKind resume_kind = async_resume_kind_;
    std::size_t resume_epc = async_resume_epc_;
    async_task_.reset();
    async_ready_ = false;
    async_resume_kind_ = AsyncResumeKind::None;
    async_resume_epc_ = 0;
    async_result_ = ScriptResult{};
    return apply_call_result(result, resume_kind, resume_epc);
}

} // namespace fiber::script::run
