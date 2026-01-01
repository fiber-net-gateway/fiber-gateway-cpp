#include "InterpreterVm.h"

#include <map>
#include <set>
#include <string>
#include <utility>

#include "../Library.h"
#include "Access.h"
#include "Binaries.h"
#include "Compares.h"
#include "../../common/json/JsGc.h"
#include "Unaries.h"
#include "../Runtime.h"

namespace fiber::script::run {

namespace {

VmError make_error(std::string name, std::string message, std::int64_t position) {
    VmError error;
    error.name = std::move(name);
    error.message = std::move(message);
    error.position = position;
    return error;
}

VmError make_oom(std::int64_t position) {
    return make_error("EXEC_OUT_OF_MEMORY", "out of memory", position);
}

bool value_to_string(const fiber::json::JsValue &value, std::string &out) {
    if (value.type_ == fiber::json::JsNodeType::NativeString) {
        out.assign(value.ns.data, value.ns.len);
        return true;
    }
    if (value.type_ == fiber::json::JsNodeType::HeapString) {
        auto *str = reinterpret_cast<const fiber::json::GcString *>(value.gc);
        return fiber::json::gc_string_to_utf8(str, out);
    }
    return false;
}

bool value_to_status(const fiber::json::JsValue &value, int &out) {
    switch (value.type_) {
        case fiber::json::JsNodeType::Integer:
            out = static_cast<int>(value.i);
            return true;
        case fiber::json::JsNodeType::Float:
            out = static_cast<int>(value.f);
            return true;
        case fiber::json::JsNodeType::Boolean:
            out = value.b ? 1 : 0;
            return true;
        case fiber::json::JsNodeType::Null:
            out = 0;
            return true;
        default:
            return false;
    }
}

} // namespace

InterpreterVm::InterpreterVm(const ir::Compiled &compiled,
                             const fiber::json::JsValue &root,
                             void *attach,
                             async::IScheduler *scheduler,
                             ScriptRuntime &runtime)
    : compiled_(compiled),
      root_(root),
      attach_(attach),
      scheduler_(scheduler),
      runtime_(runtime) {
    stack_.resize(compiled_.stack_size);
    vars_.resize(compiled_.var_table_size);
    const_cache_.resize(compiled_.operands.size());
    const_cache_valid_.resize(compiled_.operands.size(), false);
    build_exception_index();
    runtime_.roots().add_provider(this);
}

InterpreterVm::~InterpreterVm() {
    runtime_.roots().remove_provider(this);
}

async::Task<VmResult> InterpreterVm::exec_async() {
    reset_state();
    const auto &codes = compiled_.codes;
    while (pc_ < codes.size()) {
        std::int32_t instr = codes[pc_++];
        std::uint8_t op = static_cast<std::uint8_t>(instr & 0xFF);
        switch (op) {
            case ir::Code::NOOP:
                break;
            case ir::Code::LOAD_CONST: {
                std::size_t idx = static_cast<std::size_t>(instr >> 8);
                VmResult loaded = load_const(idx);
                if (!loaded) {
                    if (!handle_error(loaded.error(), pc_ - 1)) {
                        co_return std::unexpected(loaded.error());
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
                    VmResult result = Binaries::plus(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            co_return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::BOP_MINUS:
                --sp_;
                {
                    VmResult result = Binaries::minus(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            co_return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::BOP_MULTIPLY:
                --sp_;
                {
                    VmResult result = Binaries::multiply(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            co_return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::BOP_DIVIDE:
                --sp_;
                {
                    VmResult result = Binaries::divide(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            co_return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::BOP_MOD:
                --sp_;
                {
                    VmResult result = Binaries::modulo(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            co_return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::BOP_MATCH:
                --sp_;
                {
                    VmResult result = Binaries::matches(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            co_return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::BOP_LT:
                --sp_;
                {
                    VmResult result = Binaries::lt(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            co_return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::BOP_LTE:
                --sp_;
                {
                    VmResult result = Binaries::lte(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            co_return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::BOP_GT:
                --sp_;
                {
                    VmResult result = Binaries::gt(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            co_return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::BOP_GTE:
                --sp_;
                {
                    VmResult result = Binaries::gte(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            co_return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::BOP_EQ:
                --sp_;
                {
                    VmResult result = Binaries::eq(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            co_return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::BOP_SEQ:
                --sp_;
                {
                    VmResult result = Binaries::seq(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            co_return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::BOP_NE:
                --sp_;
                {
                    VmResult result = Binaries::ne(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            co_return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::BOP_SNE:
                --sp_;
                {
                    VmResult result = Binaries::sne(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            co_return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::BOP_IN:
                --sp_;
                {
                    VmResult result = Binaries::in(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            co_return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::UNARY_PLUS:
                {
                    VmResult result = Unaries::plus(stack_[sp_ - 1]);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            co_return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::UNARY_MINUS:
                {
                    VmResult result = Unaries::minus(stack_[sp_ - 1]);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            co_return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::UNARY_NEG:
                {
                    VmResult result = Unaries::neg(stack_[sp_ - 1]);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            co_return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::UNARY_TYPEOF:
                {
                    VmResult result = Unaries::typeof_op(stack_[sp_ - 1], runtime_);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            co_return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::NEW_OBJECT: {
                maybe_collect();
                fiber::json::JsValue obj = fiber::json::JsValue::make_object(runtime_.heap(), 0);
                if (obj.type_ != fiber::json::JsNodeType::Object) {
                    VmError error = make_oom(compiled_.positions[pc_ - 1]);
                    if (!handle_error(error, pc_ - 1)) {
                        co_return std::unexpected(error);
                    }
                    continue;
                }
                stack_[sp_++] = obj;
                break;
            }
            case ir::Code::NEW_ARRAY: {
                maybe_collect();
                fiber::json::JsValue arr = fiber::json::JsValue::make_array(runtime_.heap(), 0);
                if (arr.type_ != fiber::json::JsNodeType::Array) {
                    VmError error = make_oom(compiled_.positions[pc_ - 1]);
                    if (!handle_error(error, pc_ - 1)) {
                        co_return std::unexpected(error);
                    }
                    continue;
                }
                stack_[sp_++] = arr;
                break;
            }
            case ir::Code::EXP_OBJECT:
                --sp_;
                {
                    VmResult result = Access::expand_object(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            co_return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::EXP_ARRAY:
                --sp_;
                {
                    VmResult result = Access::expand_array(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            co_return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::PUSH_ARRAY:
                --sp_;
                {
                    VmResult result = Access::push_array(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            co_return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::IDX_GET:
                --sp_;
                {
                    VmResult result = Access::index_get(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            co_return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::IDX_SET:
                sp_ -= 2;
                {
                    VmResult result = Access::index_set(stack_[sp_ - 1], stack_[sp_], stack_[sp_ + 1], runtime_);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            co_return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::IDX_SET_1:
                sp_ -= 2;
                {
                    VmResult result = Access::index_set1(stack_[sp_ - 1], stack_[sp_], stack_[sp_ + 1], runtime_);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            co_return std::unexpected(result.error());
                        }
                        continue;
                    }
                }
                break;
            case ir::Code::PROP_GET: {
                std::size_t idx = static_cast<std::size_t>(instr >> 8);
                const auto *name = static_cast<const std::string *>(compiled_.operands[idx]);
                fiber::json::JsValue key = fiber::json::JsValue::make_native_string(
                    const_cast<char *>(name ? name->data() : ""),
                    name ? name->size() : 0);
                VmResult result = Access::prop_get(stack_[sp_ - 1], key, runtime_);
                if (!result) {
                    if (!handle_error(result.error(), pc_ - 1)) {
                        co_return std::unexpected(result.error());
                    }
                    continue;
                }
                stack_[sp_ - 1] = result.value();
                break;
            }
            case ir::Code::PROP_SET: {
                std::size_t idx = static_cast<std::size_t>(instr >> 8);
                const auto *name = static_cast<const std::string *>(compiled_.operands[idx]);
                fiber::json::JsValue key = fiber::json::JsValue::make_native_string(
                    const_cast<char *>(name ? name->data() : ""),
                    name ? name->size() : 0);
                --sp_;
                VmResult result = Access::prop_set(stack_[sp_ - 1], stack_[sp_], key, runtime_);
                if (!result) {
                    if (!handle_error(result.error(), pc_ - 1)) {
                        co_return std::unexpected(result.error());
                    }
                    continue;
                }
                stack_[sp_ - 1] = result.value();
                break;
            }
            case ir::Code::PROP_SET_1: {
                std::size_t idx = static_cast<std::size_t>(instr >> 8);
                const auto *name = static_cast<const std::string *>(compiled_.operands[idx]);
                fiber::json::JsValue key = fiber::json::JsValue::make_native_string(
                    const_cast<char *>(name ? name->data() : ""),
                    name ? name->size() : 0);
                --sp_;
                VmResult result = Access::prop_set1(stack_[sp_ - 1], stack_[sp_], key, runtime_);
                if (!result) {
                    if (!handle_error(result.error(), pc_ - 1)) {
                        co_return std::unexpected(result.error());
                    }
                    continue;
                }
                break;
            }
            case ir::Code::CALL_FUNC: {
                std::size_t func_index = static_cast<std::size_t>(instr >> 16);
                std::size_t arg_count = static_cast<std::size_t>((instr >> 8) & 0xFF);
                auto *function = static_cast<Library::Function *>(compiled_.operands[func_index]);
                sp_ -= arg_count;
                set_args_for_ctx(sp_, arg_count);
                stack_[sp_++] = function ? function->call(*this) : fiber::json::JsValue::make_undefined();
                break;
            }
            case ir::Code::CALL_FUNC_SPREAD: {
                std::size_t func_index = static_cast<std::size_t>(instr >> 8);
                auto *function = static_cast<Library::Function *>(compiled_.operands[func_index]);
                set_args_for_ctx(stack_[sp_ - 1]);
                stack_[sp_ - 1] = function ? function->call(*this) : fiber::json::JsValue::make_undefined();
                clear_args();
                break;
            }
            case ir::Code::CALL_ASYNC_FUNC: {
                std::size_t func_index = static_cast<std::size_t>(instr >> 16);
                std::size_t arg_count = static_cast<std::size_t>((instr >> 8) & 0xFF);
                auto *function = static_cast<Library::AsyncFunction *>(compiled_.operands[func_index]);
                sp_ -= arg_count;
                set_args_for_ctx(sp_, arg_count);
                if (!function) {
                    VmError error = make_error("EXEC_FUNC_NOT_FOUND", "async function not found",
                                               compiled_.positions[pc_ - 1]);
                    if (!handle_error(error, pc_ - 1)) {
                        co_return std::unexpected(error);
                    }
                    continue;
                }
                async::Task<fiber::json::JsValue> task = function->call(*this);
                if (scheduler_) {
                    task.set_scheduler(scheduler_);
                }
                auto result = co_await task;
                if (!result) {
                    VmError error = make_error("EXEC_ASYNC_ERROR", result.error().message, compiled_.positions[pc_ - 1]);
                    if (!handle_error(error, pc_ - 1)) {
                        co_return std::unexpected(error);
                    }
                    continue;
                }
                stack_[sp_++] = result.value();
                break;
            }
            case ir::Code::CALL_ASYNC_FUNC_SPREAD: {
                std::size_t func_index = static_cast<std::size_t>(instr >> 8);
                auto *function = static_cast<Library::AsyncFunction *>(compiled_.operands[func_index]);
                set_args_for_ctx(stack_[sp_ - 1]);
                if (!function) {
                    VmError error = make_error("EXEC_FUNC_NOT_FOUND", "async function not found",
                                               compiled_.positions[pc_ - 1]);
                    clear_args();
                    if (!handle_error(error, pc_ - 1)) {
                        co_return std::unexpected(error);
                    }
                    continue;
                }
                async::Task<fiber::json::JsValue> task = function->call(*this);
                if (scheduler_) {
                    task.set_scheduler(scheduler_);
                }
                auto result = co_await task;
                clear_args();
                if (!result) {
                    VmError error = make_error("EXEC_ASYNC_ERROR", result.error().message, compiled_.positions[pc_ - 1]);
                    if (!handle_error(error, pc_ - 1)) {
                        co_return std::unexpected(error);
                    }
                    continue;
                }
                stack_[sp_ - 1] = result.value();
                break;
            }
            case ir::Code::CALL_CONST: {
                std::size_t const_index = static_cast<std::size_t>(instr >> 8);
                auto *constant = static_cast<Library::Constant *>(compiled_.operands[const_index]);
                stack_[sp_++] = constant ? constant->get(*this) : fiber::json::JsValue::make_undefined();
                break;
            }
            case ir::Code::CALL_ASYNC_CONST: {
                std::size_t const_index = static_cast<std::size_t>(instr >> 8);
                auto *constant = static_cast<Library::AsyncConstant *>(compiled_.operands[const_index]);
                if (!constant) {
                    VmError error = make_error("EXEC_CONST_NOT_FOUND", "async constant not found",
                                               compiled_.positions[pc_ - 1]);
                    if (!handle_error(error, pc_ - 1)) {
                        co_return std::unexpected(error);
                    }
                    continue;
                }
                async::Task<fiber::json::JsValue> task = constant->get(*this);
                if (scheduler_) {
                    task.set_scheduler(scheduler_);
                }
                auto result = co_await task;
                if (!result) {
                    VmError error = make_error("EXEC_ASYNC_ERROR", result.error().message, compiled_.positions[pc_ - 1]);
                    if (!handle_error(error, pc_ - 1)) {
                        co_return std::unexpected(error);
                    }
                    continue;
                }
                stack_[sp_++] = result.value();
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
                VmResult result = Unaries::iterate(stack_[--sp_], runtime_);
                if (!result) {
                    if (!handle_error(result.error(), pc_ - 1)) {
                        co_return std::unexpected(result.error());
                    }
                    continue;
                }
                vars_[idx] = result.value();
                break;
            }
            case ir::Code::ITERATE_NEXT: {
                std::size_t idx = static_cast<std::size_t>(instr >> kInstrumentLen);
                auto *iter = reinterpret_cast<fiber::json::GcIterator *>(vars_[idx].gc);
                fiber::json::JsValue out;
                bool done = true;
                bool ok = fiber::json::gc_iterator_next(&runtime_.heap(), iter, out, done);
                stack_[sp_++] = fiber::json::JsValue::make_boolean(ok && !done);
                break;
            }
            case ir::Code::ITERATE_KEY: {
                std::size_t var_idx = static_cast<std::size_t>((instr >> kInstrumentLen) & kMaxIteratorVar);
                std::size_t iter_idx = static_cast<std::size_t>(instr >> kIteratorOff);
                auto *iter = reinterpret_cast<fiber::json::GcIterator *>(vars_[iter_idx].gc);
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
                auto *iter = reinterpret_cast<fiber::json::GcIterator *>(vars_[iter_idx].gc);
                if (iter && iter->has_current) {
                    vars_[var_idx] = iter->current_value;
                } else {
                    vars_[var_idx] = fiber::json::JsValue::make_undefined();
                }
                break;
            }
            case ir::Code::INTO_CATCH: {
                std::size_t idx = static_cast<std::size_t>(instr >> kInstrumentLen);
                VmResult exc = make_exception_value(pending_error_);
                if (!exc) {
                    if (!handle_error(exc.error(), pc_ - 1)) {
                        co_return std::unexpected(exc.error());
                    }
                    continue;
                }
                vars_[idx] = exc.value();
                has_error_ = false;
                pending_error_ = VmError{};
                break;
            }
            case ir::Code::END_RETURN: {
                if (sp_ > 0) {
                    return_value_ = stack_[sp_ - 1];
                } else {
                    return_value_ = fiber::json::JsValue::make_undefined();
                }
                has_return_ = true;
                co_return return_value_;
            }
            case ir::Code::THROW_EXP: {
                fiber::json::JsValue thrown = stack_[--sp_];
                VmError error = make_error_from_value(thrown);
                if (!handle_error(error, pc_ - 1)) {
                    co_return std::unexpected(error);
                }
                break;
            }
            default: {
                VmError error = make_error("EXEC_UNKNOWN_OPCODE", "unknown opcode", compiled_.positions[pc_ - 1]);
                if (!handle_error(error, pc_ - 1)) {
                    co_return std::unexpected(error);
                }
                break;
            }
        }
    }
    if (has_return_) {
        co_return return_value_;
    }
    co_return std::unexpected(make_error("EXEC_NO_RETURN", "no return instruction", -1));
}

VmResult InterpreterVm::exec_sync() {
    if (compiled_.contains_async()) {
        return std::unexpected(make_error("EXEC_ASYNC_REQUIRED", "async opcode in sync execution", -1));
    }
    reset_state();
    const auto &codes = compiled_.codes;
    while (pc_ < codes.size()) {
        std::int32_t instr = codes[pc_++];
        std::uint8_t op = static_cast<std::uint8_t>(instr & 0xFF);
        switch (op) {
            case ir::Code::NOOP:
                break;
            case ir::Code::LOAD_CONST: {
                std::size_t idx = static_cast<std::size_t>(instr >> 8);
                VmResult loaded = load_const(idx);
                if (!loaded) {
                    if (!handle_error(loaded.error(), pc_ - 1)) {
                        return std::unexpected(loaded.error());
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
                    VmResult result = Binaries::plus(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::BOP_MINUS:
                --sp_;
                {
                    VmResult result = Binaries::minus(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::BOP_MULTIPLY:
                --sp_;
                {
                    VmResult result = Binaries::multiply(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::BOP_DIVIDE:
                --sp_;
                {
                    VmResult result = Binaries::divide(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::BOP_MOD:
                --sp_;
                {
                    VmResult result = Binaries::modulo(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::BOP_MATCH:
                --sp_;
                {
                    VmResult result = Binaries::matches(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::BOP_LT:
                --sp_;
                {
                    VmResult result = Binaries::lt(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::BOP_LTE:
                --sp_;
                {
                    VmResult result = Binaries::lte(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::BOP_GT:
                --sp_;
                {
                    VmResult result = Binaries::gt(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::BOP_GTE:
                --sp_;
                {
                    VmResult result = Binaries::gte(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::BOP_EQ:
                --sp_;
                {
                    VmResult result = Binaries::eq(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::BOP_SEQ:
                --sp_;
                {
                    VmResult result = Binaries::seq(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::BOP_NE:
                --sp_;
                {
                    VmResult result = Binaries::ne(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::BOP_SNE:
                --sp_;
                {
                    VmResult result = Binaries::sne(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::BOP_IN:
                --sp_;
                {
                    VmResult result = Binaries::in(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::UNARY_PLUS:
                {
                    VmResult result = Unaries::plus(stack_[sp_ - 1]);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::UNARY_MINUS:
                {
                    VmResult result = Unaries::minus(stack_[sp_ - 1]);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::UNARY_NEG:
                {
                    VmResult result = Unaries::neg(stack_[sp_ - 1]);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::UNARY_TYPEOF:
                {
                    VmResult result = Unaries::typeof_op(stack_[sp_ - 1], runtime_);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::NEW_OBJECT: {
                maybe_collect();
                fiber::json::JsValue obj = fiber::json::JsValue::make_object(runtime_.heap(), 0);
                if (obj.type_ != fiber::json::JsNodeType::Object) {
                    VmError error = make_oom(compiled_.positions[pc_ - 1]);
                    if (!handle_error(error, pc_ - 1)) {
                        return std::unexpected(error);
                    }
                    continue;
                }
                stack_[sp_++] = obj;
                break;
            }
            case ir::Code::NEW_ARRAY: {
                maybe_collect();
                fiber::json::JsValue arr = fiber::json::JsValue::make_array(runtime_.heap(), 0);
                if (arr.type_ != fiber::json::JsNodeType::Array) {
                    VmError error = make_oom(compiled_.positions[pc_ - 1]);
                    if (!handle_error(error, pc_ - 1)) {
                        return std::unexpected(error);
                    }
                    continue;
                }
                stack_[sp_++] = arr;
                break;
            }
            case ir::Code::EXP_OBJECT:
                --sp_;
                {
                    VmResult result = Access::expand_object(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::EXP_ARRAY:
                --sp_;
                {
                    VmResult result = Access::expand_array(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::PUSH_ARRAY:
                --sp_;
                {
                    VmResult result = Access::push_array(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::IDX_GET:
                --sp_;
                {
                    VmResult result = Access::index_get(stack_[sp_ - 1], stack_[sp_], runtime_);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::IDX_SET:
                sp_ -= 2;
                {
                    VmResult result = Access::index_set(stack_[sp_ - 1], stack_[sp_], stack_[sp_ + 1], runtime_);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            return std::unexpected(result.error());
                        }
                        continue;
                    }
                    stack_[sp_ - 1] = result.value();
                }
                break;
            case ir::Code::IDX_SET_1:
                sp_ -= 2;
                {
                    VmResult result = Access::index_set1(stack_[sp_ - 1], stack_[sp_], stack_[sp_ + 1], runtime_);
                    if (!result) {
                        if (!handle_error(result.error(), pc_ - 1)) {
                            return std::unexpected(result.error());
                        }
                        continue;
                    }
                }
                break;
            case ir::Code::PROP_GET: {
                std::size_t idx = static_cast<std::size_t>(instr >> 8);
                const auto *name = static_cast<const std::string *>(compiled_.operands[idx]);
                fiber::json::JsValue key = fiber::json::JsValue::make_native_string(
                    const_cast<char *>(name ? name->data() : ""),
                    name ? name->size() : 0);
                VmResult result = Access::prop_get(stack_[sp_ - 1], key, runtime_);
                if (!result) {
                    if (!handle_error(result.error(), pc_ - 1)) {
                        return std::unexpected(result.error());
                    }
                    continue;
                }
                stack_[sp_ - 1] = result.value();
                break;
            }
            case ir::Code::PROP_SET: {
                std::size_t idx = static_cast<std::size_t>(instr >> 8);
                const auto *name = static_cast<const std::string *>(compiled_.operands[idx]);
                fiber::json::JsValue key = fiber::json::JsValue::make_native_string(
                    const_cast<char *>(name ? name->data() : ""),
                    name ? name->size() : 0);
                --sp_;
                VmResult result = Access::prop_set(stack_[sp_ - 1], stack_[sp_], key, runtime_);
                if (!result) {
                    if (!handle_error(result.error(), pc_ - 1)) {
                        return std::unexpected(result.error());
                    }
                    continue;
                }
                stack_[sp_ - 1] = result.value();
                break;
            }
            case ir::Code::PROP_SET_1: {
                std::size_t idx = static_cast<std::size_t>(instr >> 8);
                const auto *name = static_cast<const std::string *>(compiled_.operands[idx]);
                fiber::json::JsValue key = fiber::json::JsValue::make_native_string(
                    const_cast<char *>(name ? name->data() : ""),
                    name ? name->size() : 0);
                --sp_;
                VmResult result = Access::prop_set1(stack_[sp_ - 1], stack_[sp_], key, runtime_);
                if (!result) {
                    if (!handle_error(result.error(), pc_ - 1)) {
                        return std::unexpected(result.error());
                    }
                    continue;
                }
                break;
            }
            case ir::Code::CALL_FUNC: {
                std::size_t func_index = static_cast<std::size_t>(instr >> 16);
                std::size_t arg_count = static_cast<std::size_t>((instr >> 8) & 0xFF);
                auto *function = static_cast<Library::Function *>(compiled_.operands[func_index]);
                sp_ -= arg_count;
                set_args_for_ctx(sp_, arg_count);
                stack_[sp_++] = function ? function->call(*this) : fiber::json::JsValue::make_undefined();
                break;
            }
            case ir::Code::CALL_FUNC_SPREAD: {
                std::size_t func_index = static_cast<std::size_t>(instr >> 8);
                auto *function = static_cast<Library::Function *>(compiled_.operands[func_index]);
                set_args_for_ctx(stack_[sp_ - 1]);
                stack_[sp_ - 1] = function ? function->call(*this) : fiber::json::JsValue::make_undefined();
                clear_args();
                break;
            }
            case ir::Code::CALL_ASYNC_FUNC:
            case ir::Code::CALL_ASYNC_FUNC_SPREAD:
            case ir::Code::CALL_ASYNC_CONST: {
                VmError error = make_error("EXEC_ASYNC_REQUIRED", "async opcode in sync execution", compiled_.positions[pc_ - 1]);
                if (!handle_error(error, pc_ - 1)) {
                    return std::unexpected(error);
                }
                break;
            }
            case ir::Code::CALL_CONST: {
                std::size_t const_index = static_cast<std::size_t>(instr >> 8);
                auto *constant = static_cast<Library::Constant *>(compiled_.operands[const_index]);
                stack_[sp_++] = constant ? constant->get(*this) : fiber::json::JsValue::make_undefined();
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
                VmResult result = Unaries::iterate(stack_[--sp_], runtime_);
                if (!result) {
                    if (!handle_error(result.error(), pc_ - 1)) {
                        return std::unexpected(result.error());
                    }
                    continue;
                }
                vars_[idx] = result.value();
                break;
            }
            case ir::Code::ITERATE_NEXT: {
                std::size_t idx = static_cast<std::size_t>(instr >> kInstrumentLen);
                auto *iter = reinterpret_cast<fiber::json::GcIterator *>(vars_[idx].gc);
                fiber::json::JsValue out;
                bool done = true;
                bool ok = fiber::json::gc_iterator_next(&runtime_.heap(), iter, out, done);
                stack_[sp_++] = fiber::json::JsValue::make_boolean(ok && !done);
                break;
            }
            case ir::Code::ITERATE_KEY: {
                std::size_t var_idx = static_cast<std::size_t>((instr >> kInstrumentLen) & kMaxIteratorVar);
                std::size_t iter_idx = static_cast<std::size_t>(instr >> kIteratorOff);
                auto *iter = reinterpret_cast<fiber::json::GcIterator *>(vars_[iter_idx].gc);
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
                auto *iter = reinterpret_cast<fiber::json::GcIterator *>(vars_[iter_idx].gc);
                if (iter && iter->has_current) {
                    vars_[var_idx] = iter->current_value;
                } else {
                    vars_[var_idx] = fiber::json::JsValue::make_undefined();
                }
                break;
            }
            case ir::Code::INTO_CATCH: {
                std::size_t idx = static_cast<std::size_t>(instr >> kInstrumentLen);
                VmResult exc = make_exception_value(pending_error_);
                if (!exc) {
                    if (!handle_error(exc.error(), pc_ - 1)) {
                        return std::unexpected(exc.error());
                    }
                    continue;
                }
                vars_[idx] = exc.value();
                has_error_ = false;
                pending_error_ = VmError{};
                break;
            }
            case ir::Code::END_RETURN: {
                if (sp_ > 0) {
                    return_value_ = stack_[sp_ - 1];
                } else {
                    return_value_ = fiber::json::JsValue::make_undefined();
                }
                has_return_ = true;
                return return_value_;
            }
            case ir::Code::THROW_EXP: {
                fiber::json::JsValue thrown = stack_[--sp_];
                VmError error = make_error_from_value(thrown);
                if (!handle_error(error, pc_ - 1)) {
                    return std::unexpected(error);
                }
                break;
            }
            default: {
                VmError error = make_error("EXEC_UNKNOWN_OPCODE", "unknown opcode", compiled_.positions[pc_ - 1]);
                if (!handle_error(error, pc_ - 1)) {
                    return std::unexpected(error);
                }
                break;
            }
        }
    }
    if (has_return_) {
        return return_value_;
    }
    return std::unexpected(make_error("EXEC_NO_RETURN", "no return instruction", -1));
}

const fiber::json::JsValue &InterpreterVm::root() const {
    return root_;
}

void *InterpreterVm::attach() const {
    return attach_;
}

const fiber::json::JsValue &InterpreterVm::arg_value(std::size_t index) const {
    if (use_spread_args_) {
        if (spread_args_.type_ != fiber::json::JsNodeType::Array || !spread_args_.gc) {
            return undefined_;
        }
        auto *arr = reinterpret_cast<const fiber::json::GcArray *>(spread_args_.gc);
        const fiber::json::JsValue *found = fiber::json::gc_array_get(arr, index);
        return found ? *found : undefined_;
    }
    if (index >= arg_cnt_) {
        return undefined_;
    }
    std::size_t slot = arg_off_ + index;
    if (slot >= stack_.size()) {
        return undefined_;
    }
    return stack_[slot];
}

std::size_t InterpreterVm::arg_count() const {
    if (use_spread_args_) {
        if (spread_args_.type_ != fiber::json::JsNodeType::Array || !spread_args_.gc) {
            return 0;
        }
        auto *arr = reinterpret_cast<const fiber::json::GcArray *>(spread_args_.gc);
        return arr ? arr->size : 0;
    }
    return arg_cnt_;
}

async::IScheduler *InterpreterVm::scheduler() const {
    return scheduler_;
}

void InterpreterVm::visit_roots(fiber::json::GcRootSet::RootVisitor &visitor) {
    visitor.visit(&root_);
    visitor.visit_range(stack_.data(), sp_);
    visitor.visit_range(vars_.data(), vars_.size());
    if (use_spread_args_) {
        visitor.visit(&spread_args_);
    }
    for (std::size_t i = 0; i < const_cache_.size(); ++i) {
        if (const_cache_valid_[i]) {
            visitor.visit(&const_cache_[i]);
        }
    }
    if (has_error_) {
        visitor.visit(&pending_error_.meta);
    }
    if (has_return_) {
        visitor.visit(&return_value_);
    }
}

void InterpreterVm::reset_state() {
    sp_ = 0;
    pc_ = 0;
    arg_off_ = 0;
    arg_cnt_ = 0;
    use_spread_args_ = false;
    spread_args_ = fiber::json::JsValue::make_undefined();
    pending_error_ = VmError{};
    has_error_ = false;
    return_value_ = fiber::json::JsValue::make_undefined();
    has_return_ = false;
}

void InterpreterVm::set_args_for_ctx(std::size_t off, std::size_t count) {
    use_spread_args_ = false;
    arg_off_ = off;
    arg_cnt_ = count;
}

void InterpreterVm::set_args_for_ctx(const fiber::json::JsValue &args) {
    use_spread_args_ = true;
    spread_args_ = args;
}

void InterpreterVm::clear_args() {
    use_spread_args_ = false;
    spread_args_ = fiber::json::JsValue::make_undefined();
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
    if (epc < static_cast<std::size_t>(exp_ins_[0]) ||
        static_cast<std::size_t>(exp_ins_[len - 1]) <= epc) {
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
    for (const auto &entry : ranges) {
        exp_ins_[idx] = entry.first;
        exp_ins_[idx + size] = entry.second;
        ++idx;
    }
}

bool InterpreterVm::handle_error(VmError error, std::size_t epc) {
    if (error.position < 0 && epc < compiled_.positions.size()) {
        error.position = compiled_.positions[epc];
    }
    pending_error_ = std::move(error);
    has_error_ = true;
    return catch_for_exception(epc);
}

VmResult InterpreterVm::load_const(std::size_t operand_index) {
    if (operand_index >= compiled_.operands.size()) {
        return std::unexpected(make_error("EXEC_BAD_CONST", "constant index out of range", -1));
    }
    if (const_cache_valid_[operand_index]) {
        return const_cache_[operand_index];
    }
    const auto *cv = static_cast<const ir::Compiled::ConstValue *>(compiled_.operands[operand_index]);
    if (!cv) {
        return std::unexpected(make_error("EXEC_BAD_CONST", "null constant", -1));
    }
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
            maybe_collect();
            value = fiber::json::JsValue::make_string(runtime_.heap(), cv->text.data(), cv->text.size());
            if (value.type_ != fiber::json::JsNodeType::HeapString) {
                return std::unexpected(make_oom(-1));
            }
            break;
        }
        case ir::Compiled::ConstValue::Kind::Binary: {
            maybe_collect();
            value = fiber::json::JsValue::make_binary(runtime_.heap(), cv->bytes.data(), cv->bytes.size());
            if (value.type_ != fiber::json::JsNodeType::HeapBinary) {
                return std::unexpected(make_oom(-1));
            }
            break;
        }
    }
    const_cache_[operand_index] = value;
    const_cache_valid_[operand_index] = true;
    return value;
}

VmResult InterpreterVm::make_exception_value(const VmError &error) {
    maybe_collect();
    std::string name = error.name.empty() ? "EXEC_ERROR" : error.name;
    std::string message = error.message.empty() ? "script error" : error.message;
    fiber::json::GcString *name_str = fiber::json::gc_new_string(&runtime_.heap(), name.c_str(), name.size());
    if (!name_str) {
        return std::unexpected(make_oom(error.position));
    }
    fiber::json::GcString *message_str = fiber::json::gc_new_string(&runtime_.heap(), message.c_str(), message.size());
    if (!message_str) {
        return std::unexpected(make_oom(error.position));
    }
    fiber::json::GcException *exc =
        fiber::json::gc_new_exception(&runtime_.heap(), error.position, name_str, message_str, error.meta);
    if (!exc) {
        return std::unexpected(make_oom(error.position));
    }
    fiber::json::JsValue result;
    result.type_ = fiber::json::JsNodeType::Exception;
    result.gc = &exc->hdr;
    return result;
}

VmError InterpreterVm::make_error_from_value(const fiber::json::JsValue &value) {
    VmError error;
    error.name = "EXEC_THROW_ERROR";
    error.message = "execute script throw error";
    error.status = 500;
    error.meta = value;
    if (value.type_ == fiber::json::JsNodeType::Exception) {
        auto *exc = reinterpret_cast<const fiber::json::GcException *>(value.gc);
        if (exc) {
            error.position = exc->position;
            std::string name;
            if (exc->name && fiber::json::gc_string_to_utf8(exc->name, name)) {
                error.name = name;
            }
            std::string message;
            if (exc->message && fiber::json::gc_string_to_utf8(exc->message, message)) {
                error.message = message;
            }
            error.meta = exc->meta;
        }
        return error;
    }
    if (value.type_ == fiber::json::JsNodeType::Object && value.gc) {
        maybe_collect();
        auto *obj = reinterpret_cast<const fiber::json::GcObject *>(value.gc);
        const char *keys[] = {"name", "message", "status", "meta"};
        fiber::json::GcString *name_key = fiber::json::gc_new_string(&runtime_.heap(), keys[0], 4);
        fiber::json::GcString *msg_key = fiber::json::gc_new_string(&runtime_.heap(), keys[1], 7);
        fiber::json::GcString *status_key = fiber::json::gc_new_string(&runtime_.heap(), keys[2], 6);
        fiber::json::GcString *meta_key = fiber::json::gc_new_string(&runtime_.heap(), keys[3], 4);
        const fiber::json::JsValue *name_val = name_key ? fiber::json::gc_object_get(obj, name_key) : nullptr;
        const fiber::json::JsValue *msg_val = msg_key ? fiber::json::gc_object_get(obj, msg_key) : nullptr;
        const fiber::json::JsValue *status_val = status_key ? fiber::json::gc_object_get(obj, status_key) : nullptr;
        const fiber::json::JsValue *meta_val = meta_key ? fiber::json::gc_object_get(obj, meta_key) : nullptr;
        if (name_val) {
            std::string text;
            if (value_to_string(*name_val, text)) {
                error.name = text;
            }
        }
        if (msg_val) {
            std::string text;
            if (value_to_string(*msg_val, text)) {
                error.message = text;
            }
        }
        if (status_val) {
            int status = 0;
            if (value_to_status(*status_val, status)) {
                error.status = status;
            }
        }
        if (meta_val) {
            error.meta = *meta_val;
        }
    }
    return error;
}

bool InterpreterVm::maybe_collect() {
    runtime_.maybe_collect();
    return true;
}

} // namespace fiber::script::run
