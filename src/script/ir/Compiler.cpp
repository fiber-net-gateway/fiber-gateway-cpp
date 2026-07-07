#include "Compiler.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../../common/Assert.h"
#include "../ast/Assign.h"
#include "../ast/BinaryOperator.h"
#include "../ast/Block.h"
#include "../ast/BreakStatement.h"
#include "../ast/ConstantVal.h"
#include "../ast/ContinueStatement.h"
#include "../ast/DirectiveStatement.h"
#include "../ast/ExpandArrArg.h"
#include "../ast/ExpressionStatement.h"
#include "../ast/ForeachStatement.h"
#include "../ast/FunctionCall.h"
#include "../ast/Identifier.h"
#include "../ast/IfStatement.h"
#include "../ast/Indexer.h"
#include "../ast/InlineList.h"
#include "../ast/InlineObject.h"
#include "../ast/Literal.h"
#include "../ast/LogicRelationalExpression.h"
#include "../ast/PropertyReference.h"
#include "../ast/ReturnStatement.h"
#include "../ast/Statement.h"
#include "../ast/Ternary.h"
#include "../ast/ThrowStatement.h"
#include "../ast/TryCatchStatement.h"
#include "../ast/UnaryOperator.h"
#include "../ast/VariableDeclareStatement.h"
#include "../ast/VariableReference.h"

namespace fiber::script::ir {

class CompilerImpl {
public:
    Compiled compile(const ast::Node &node) {
        push_scope();
        if (auto *block = dynamic_cast<const ast::Block *>(&node)) {
            compile_block(*block, false);
            emit_default_return(block->end_pos());
        } else if (auto *expr = dynamic_cast<const ast::Expression *>(&node)) {
            compile_expression(*expr);
            emit_end_return(expr->end_pos());
        } else if (auto *stmt = dynamic_cast<const ast::Statement *>(&node)) {
            compile_statement(*stmt);
            emit_default_return(stmt->end_pos());
        }
        pop_scope();
        std::size_t stack_size = max_stack_ > 0 ? static_cast<std::size_t>(max_stack_) : 1;
        return Compiled::build(stack_size, next_var_index_, codes_, positions_, constants_, func_consts_,
                               exception_table_, payload_);
    }

private:
    struct Scope {
        std::unordered_map<std::string, std::size_t> vars;
    };

    struct LoopContext {
        std::size_t continue_target = 0;
        std::vector<std::size_t> break_jumps;
        std::vector<std::size_t> continue_jumps;
    };

    static constexpr int kInstrumentLen = 8;
    static constexpr int kIteratorLen = 12;
    static constexpr int kIteratorOff = kInstrumentLen + kIteratorLen;
    static constexpr std::size_t kMaxIteratorVar = (1u << kIteratorLen) - 1u;
    static constexpr std::uint32_t kMaxOperand24 = (1u << 24u) - 1u;
    static constexpr std::uint32_t kMaxFuncIndex16 = (1u << 16u) - 1u;
    static constexpr std::uint32_t kMaxArgCount8 = (1u << 8u) - 1u;

    std::vector<std::int32_t> codes_;
    std::vector<std::int32_t> positions_;
    std::vector<Compiled::ConstantInit> constants_;
    std::vector<Compiled::FuncConst> func_consts_;
    std::vector<std::uint32_t> exception_table_;
    std::vector<std::byte> payload_;
    std::vector<Scope> scopes_;
    std::vector<LoopContext> loops_;
    std::unordered_map<const Library::HostCallable *, std::size_t> func_const_cache_;
    std::unordered_map<std::string, std::size_t> string_constants_;
    std::optional<std::size_t> undef_const_;
    std::optional<std::size_t> null_const_;
    std::optional<std::size_t> true_const_;
    std::optional<std::size_t> false_const_;
    std::size_t next_var_index_ = 0;
    int stack_depth_ = 0;
    int max_stack_ = 0;

    void push_scope() { scopes_.push_back(Scope{}); }

    void pop_scope() {
        if (!scopes_.empty()) {
            scopes_.pop_back();
        }
    }

    std::size_t declare_var(const std::string &name) {
        if (scopes_.empty()) {
            push_scope();
        }
        auto &scope = scopes_.back();
        auto it = scope.vars.find(name);
        if (it != scope.vars.end()) {
            return it->second;
        }
        std::size_t index = next_var_index_++;
        scope.vars.emplace(name, index);
        return index;
    }

    std::size_t resolve_var(const std::string &name) {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto found = it->vars.find(name);
            if (found != it->vars.end()) {
                return found->second;
            }
        }
        return declare_var(name);
    }

    std::size_t reserve_temp_var() { return next_var_index_++; }

    void update_stack(int delta) {
        stack_depth_ += delta;
        if (stack_depth_ < 0) {
            stack_depth_ = 0;
        }
        if (stack_depth_ > max_stack_) {
            max_stack_ = stack_depth_;
        }
    }

    std::size_t emit_raw(std::int32_t code, std::int32_t pos, int delta) {
        codes_.push_back(code);
        positions_.push_back(pos);
        update_stack(delta);
        return codes_.size() - 1;
    }

    std::size_t emit_op(std::uint8_t op, std::size_t operand, std::int32_t pos, int delta) {
        FIBER_ASSERT(operand <= kMaxOperand24);
        std::uint32_t code = static_cast<std::uint32_t>(op) | (static_cast<std::uint32_t>(operand) << 8u);
        return emit_raw(static_cast<std::int32_t>(code), pos, delta);
    }

    std::size_t emit_func_call(std::uint8_t op, std::size_t func_index, std::size_t argc, std::int32_t pos, int delta) {
        FIBER_ASSERT(func_index <= kMaxFuncIndex16);
        FIBER_ASSERT(argc <= kMaxArgCount8);
        std::uint32_t code = static_cast<std::uint32_t>(op) | (static_cast<std::uint32_t>(argc) << 8u) |
                             (static_cast<std::uint32_t>(func_index) << 16u);
        return emit_raw(static_cast<std::int32_t>(code), pos, delta);
    }

    std::size_t emit_jump(std::uint8_t op, std::size_t target, std::int32_t pos) {
        int delta = 0;
        if (op == Code::JUMP_IF_FALSE || op == Code::JUMP_IF_TRUE) {
            delta = -1;
        }
        return emit_op(op, target, pos, delta);
    }

    void patch_jump(std::size_t index, std::size_t target) {
        FIBER_ASSERT(target <= kMaxOperand24);
        std::uint32_t code = static_cast<std::uint32_t>(codes_[index]);
        std::uint8_t op = static_cast<std::uint8_t>(code & 0xFFu);
        codes_[index] =
                static_cast<std::int32_t>(static_cast<std::uint32_t>(op) | (static_cast<std::uint32_t>(target) << 8u));
    }

    std::size_t add_func_const(const Library::HostCallable *callable) {
        FIBER_ASSERT(callable);
        auto it = func_const_cache_.find(callable);
        if (it != func_const_cache_.end()) {
            return it->second;
        }
        Compiled::FuncConst func;
        func.user_data = callable->userdata;
        switch (callable->kind) {
            case Library::HostCallable::Kind::SyncFunction:
                FIBER_ASSERT(callable->function);
                func.sync_func = callable->function;
                break;
            case Library::HostCallable::Kind::AsyncFunction:
                FIBER_ASSERT(callable->async_function);
                func.async_func = callable->async_function;
                break;
            case Library::HostCallable::Kind::SyncConstant:
                FIBER_ASSERT(callable->constant);
                func.sync_ct = callable->constant;
                break;
            case Library::HostCallable::Kind::AsyncConstant:
                FIBER_ASSERT(callable->async_constant);
                func.async_ct = callable->async_constant;
                break;
        }
        func_consts_.push_back(func);
        std::size_t index = func_consts_.size() - 1;
        func_const_cache_.emplace(callable, index);
        return index;
    }

    std::uint32_t append_payload(const void *data, std::size_t len) {
        FIBER_ASSERT(len == 0 || data != nullptr);
        FIBER_ASSERT(payload_.size() <= kMaxOperand24);
        std::uint32_t offset = static_cast<std::uint32_t>(payload_.size());
        if (len == 0) {
            payload_.push_back(std::byte{0});
            return offset;
        }
        const auto *bytes = static_cast<const std::byte *>(data);
        payload_.insert(payload_.end(), bytes, bytes + len);
        return offset;
    }

    std::size_t add_string_constant(std::string_view value) {
        std::string key(value);
        auto it = string_constants_.find(key);
        if (it != string_constants_.end()) {
            return it->second;
        }
        Compiled::ConstantInit init;
        init.value = fiber::script::JsValue::make_native_string(nullptr, value.size());
        init.payload_offset = append_payload(value.data(), value.size());
        constants_.push_back(init);
        std::size_t index = constants_.size() - 1;
        string_constants_.emplace(std::move(key), index);
        return index;
    }

    std::size_t add_const_value(const fiber::script::JsValue &value) {
        Compiled::ConstantInit init;
        init.value = value;
        if (fiber::script::js_value_is_borrowed_string(value)) {
            fiber::script::NativeStr text = fiber::script::js_value_native_string(value);
            init.value = fiber::script::JsValue::make_native_string(nullptr, text.len);
            init.payload_offset = append_payload(text.data, text.len);
        } else if (fiber::script::js_value_is_borrowed_binary(value)) {
            fiber::script::NativeBin bytes = fiber::script::js_value_native_binary(value);
            init.value = fiber::script::JsValue::make_native_binary(nullptr, bytes.len);
            init.payload_offset = append_payload(bytes.data, bytes.len);
        } else {
            FIBER_ASSERT(fiber::script::js_value_type(value) != fiber::script::JsNodeType::String);
            FIBER_ASSERT(fiber::script::js_value_type(value) != fiber::script::JsNodeType::Binary);
        }
        constants_.push_back(init);
        return constants_.size() - 1;
    }

    std::size_t const_undefined() {
        if (undef_const_) {
            return *undef_const_;
        }
        std::size_t idx = add_const_value(fiber::script::JsValue::make_undefined());
        undef_const_ = idx;
        return idx;
    }

    std::size_t const_null() {
        if (null_const_) {
            return *null_const_;
        }
        std::size_t idx = add_const_value(fiber::script::JsValue::make_null());
        null_const_ = idx;
        return idx;
    }

    std::size_t const_bool(bool value) {
        if (value && true_const_) {
            return *true_const_;
        }
        if (!value && false_const_) {
            return *false_const_;
        }
        std::size_t idx = add_const_value(fiber::script::JsValue::make_boolean(value));
        if (value) {
            true_const_ = idx;
        } else {
            false_const_ = idx;
        }
        return idx;
    }

    std::size_t const_js_value(const fiber::script::JsValue &value) {
        switch (fiber::script::js_value_type(value)) {
            case fiber::script::JsNodeType::Undefined:
                return const_undefined();
            case fiber::script::JsNodeType::Null:
                return const_null();
            case fiber::script::JsNodeType::Boolean:
                return const_bool(fiber::script::js_value_bool(value));
            case fiber::script::JsNodeType::Integer:
            case fiber::script::JsNodeType::Float:
            case fiber::script::JsNodeType::String:
            case fiber::script::JsNodeType::Binary:
                return add_const_value(value);
            case fiber::script::JsNodeType::Array:
            case fiber::script::JsNodeType::Object:
            case fiber::script::JsNodeType::Interator:
            case fiber::script::JsNodeType::Exception:
                FIBER_ASSERT(false);
                return const_undefined();
        }
        return const_undefined();
    }

    void emit_load_js_value(const fiber::script::JsValue &value, std::int32_t pos) {
        emit_op(Code::LOAD_CONST, const_js_value(value), pos, 1);
    }

    void emit_default_return(std::int32_t pos) {
        emit_op(Code::LOAD_CONST, const_undefined(), pos, 1);
        emit_end_return(pos);
    }

    void emit_end_return(std::int32_t pos) {
        emit_raw(static_cast<std::int32_t>(Code::END_RETURN), pos, 0);
        stack_depth_ = 0;
    }

    void compile_block(const ast::Block &block, bool push_new_scope) {
        if (push_new_scope) {
            push_scope();
        }
        for (const auto &stmt: block.statements()) {
            if (stmt) {
                compile_statement(*stmt);
            }
        }
        if (push_new_scope) {
            pop_scope();
        }
    }

    void compile_statement(const ast::Statement &stmt) {
        if (auto *block = dynamic_cast<const ast::Block *>(&stmt)) {
            compile_block(*block, true);
            return;
        }
        if (auto *expr_stmt = dynamic_cast<const ast::ExpressionStatement *>(&stmt)) {
            if (expr_stmt->expression()) {
                compile_expression(*expr_stmt->expression());
                emit_raw(static_cast<std::int32_t>(Code::POP), stmt.start_pos(), -1);
            }
            return;
        }
        if (auto *var_stmt = dynamic_cast<const ast::VariableDeclareStatement *>(&stmt)) {
            std::size_t var_idx = declare_var(var_stmt->identifier()->name());
            if (var_stmt->initializer()) {
                compile_expression(*var_stmt->initializer());
            } else {
                emit_op(Code::LOAD_CONST, const_undefined(), stmt.start_pos(), 1);
            }
            emit_op(Code::STORE_VAR, var_idx, stmt.start_pos(), -1);
            return;
        }
        if (auto *ret_stmt = dynamic_cast<const ast::ReturnStatement *>(&stmt)) {
            if (ret_stmt->value()) {
                compile_expression(*ret_stmt->value());
            } else {
                emit_op(Code::LOAD_CONST, const_undefined(), stmt.start_pos(), 1);
            }
            emit_end_return(stmt.start_pos());
            return;
        }
        if (auto *throw_stmt = dynamic_cast<const ast::ThrowStatement *>(&stmt)) {
            if (throw_stmt->value()) {
                compile_expression(*throw_stmt->value());
                emit_raw(static_cast<std::int32_t>(Code::THROW_EXP), stmt.start_pos(), -1);
                stack_depth_ = 0;
            }
            return;
        }
        if (auto *if_stmt = dynamic_cast<const ast::IfStatement *>(&stmt)) {
            compile_expression(*if_stmt->condition());
            std::size_t else_jump = emit_jump(Code::JUMP_IF_FALSE, 0, stmt.start_pos());
            int saved_depth = stack_depth_;
            if (if_stmt->then_branch()) {
                compile_statement(*if_stmt->then_branch());
            }
            std::size_t end_jump = emit_jump(Code::JUMP, 0, stmt.start_pos());
            std::size_t else_target = codes_.size();
            patch_jump(else_jump, else_target);
            stack_depth_ = saved_depth;
            if (if_stmt->else_branch()) {
                compile_statement(*if_stmt->else_branch());
            }
            std::size_t end_target = codes_.size();
            patch_jump(end_jump, end_target);
            return;
        }
        if (auto *foreach_stmt = dynamic_cast<const ast::ForeachStatement *>(&stmt)) {
            compile_expression(*foreach_stmt->collection());
            std::size_t iter_idx = reserve_temp_var();
            emit_op(Code::ITERATE_INTO, iter_idx, stmt.start_pos(), -1);

            push_scope();
            std::size_t key_idx = declare_var(foreach_stmt->key()->name());
            std::size_t value_idx = declare_var(foreach_stmt->value()->name());

            std::size_t loop_start = codes_.size();
            emit_op(Code::ITERATE_NEXT, iter_idx, stmt.start_pos(), 1);
            std::size_t exit_jump = emit_jump(Code::JUMP_IF_FALSE, 0, stmt.start_pos());

            std::size_t key_target = key_idx;
            std::size_t val_target = value_idx;
            if (key_target > kMaxIteratorVar) {
                key_target = kMaxIteratorVar;
            }
            if (val_target > kMaxIteratorVar) {
                val_target = kMaxIteratorVar;
            }
            std::size_t iter_target = iter_idx;
            if (iter_target > kMaxIteratorVar) {
                iter_target = kMaxIteratorVar;
            }
            std::int32_t key_code = static_cast<std::int32_t>(Code::ITERATE_KEY) |
                                    (static_cast<std::int32_t>(key_target) << kInstrumentLen) |
                                    (static_cast<std::int32_t>(iter_target) << kIteratorOff);
            emit_raw(key_code, stmt.start_pos(), 0);
            std::int32_t value_code = static_cast<std::int32_t>(Code::ITERATE_VALUE) |
                                      (static_cast<std::int32_t>(val_target) << kInstrumentLen) |
                                      (static_cast<std::int32_t>(iter_target) << kIteratorOff);
            emit_raw(value_code, stmt.start_pos(), 0);

            LoopContext loop;
            loop.continue_target = loop_start;
            loops_.push_back(loop);
            if (foreach_stmt->block()) {
                compile_block(*foreach_stmt->block(), false);
            }
            LoopContext finished = loops_.back();
            loops_.pop_back();

            emit_jump(Code::JUMP, loop_start, stmt.start_pos());
            std::size_t loop_end = codes_.size();
            patch_jump(exit_jump, loop_end);
            for (std::size_t jump_index: finished.break_jumps) {
                patch_jump(jump_index, loop_end);
            }
            for (std::size_t jump_index: finished.continue_jumps) {
                patch_jump(jump_index, finished.continue_target);
            }
            pop_scope();
            return;
        }
        if (auto *try_stmt = dynamic_cast<const ast::TryCatchStatement *>(&stmt)) {
            std::size_t try_begin = codes_.size();
            if (try_stmt->try_block()) {
                compile_block(*try_stmt->try_block(), true);
            }
            std::size_t jump_over = emit_jump(Code::JUMP, 0, stmt.start_pos());
            std::size_t catch_begin = codes_.size();

            push_scope();
            std::size_t catch_var = declare_var(try_stmt->identifier()->name());
            emit_op(Code::INTO_CATCH, catch_var, stmt.start_pos(), 0);
            stack_depth_ = 0;
            if (try_stmt->catch_block()) {
                compile_block(*try_stmt->catch_block(), false);
            }
            pop_scope();

            std::size_t catch_end = codes_.size();
            patch_jump(jump_over, catch_end);

            exception_table_.push_back(static_cast<std::uint32_t>(try_begin));
            exception_table_.push_back(static_cast<std::uint32_t>(catch_begin));
            exception_table_.push_back(static_cast<std::uint32_t>(catch_end));
            return;
        }
        if (auto *break_stmt = dynamic_cast<const ast::BreakStatement *>(&stmt)) {
            if (!loops_.empty()) {
                std::size_t idx = emit_jump(Code::JUMP, 0, break_stmt->start_pos());
                loops_.back().break_jumps.push_back(idx);
            }
            return;
        }
        if (auto *continue_stmt = dynamic_cast<const ast::ContinueStatement *>(&stmt)) {
            if (!loops_.empty()) {
                std::size_t idx = emit_jump(Code::JUMP, 0, continue_stmt->start_pos());
                loops_.back().continue_jumps.push_back(idx);
            }
            return;
        }
        if (dynamic_cast<const ast::DirectiveStatement *>(&stmt)) {
            return;
        }
    }

    void compile_expression(const ast::Expression &expr) {
        if (auto *literal = dynamic_cast<const ast::Literal *>(&expr)) {
            fiber::script::JsValue value = fiber::script::JsValue::make_undefined();
            switch (literal->kind()) {
                case ast::Literal::Kind::NullValue:
                    emit_op(Code::LOAD_CONST, const_null(), expr.start_pos(), 1);
                    return;
                case ast::Literal::Kind::Boolean:
                    emit_op(Code::LOAD_CONST, const_bool(literal->bool_value()), expr.start_pos(), 1);
                    return;
                case ast::Literal::Kind::Integer:
                    value = fiber::script::JsValue::make_integer(literal->int_value());
                    break;
                case ast::Literal::Kind::Float:
                    value = fiber::script::JsValue::make_float(literal->float_value());
                    break;
                case ast::Literal::Kind::String:
                    value = fiber::script::JsValue::make_native_string(literal->string_value().data(),
                                                                       literal->string_value().size());
                    break;
            }
            std::size_t idx = add_const_value(value);
            emit_op(Code::LOAD_CONST, idx, expr.start_pos(), 1);
            return;
        }
        if (auto *identifier = dynamic_cast<const ast::Identifier *>(&expr)) {
            std::size_t idx = resolve_var(identifier->name());
            emit_op(Code::LOAD_VAR, idx, expr.start_pos(), 1);
            return;
        }
        if (auto *var = dynamic_cast<const ast::VariableReference *>(&expr)) {
            if (var->is_root()) {
                emit_raw(static_cast<std::int32_t>(Code::LOAD_ROOT), expr.start_pos(), 1);
                return;
            }
            std::size_t idx = resolve_var(var->name());
            emit_op(Code::LOAD_VAR, idx, expr.start_pos(), 1);
            return;
        }
        if (auto *constant = dynamic_cast<const ast::ConstantVal *>(&expr)) {
            if (constant->is_async()) {
                FIBER_ASSERT(constant->async_constant()->kind == Library::HostCallable::Kind::AsyncConstant);
                std::size_t func_idx = add_func_const(constant->async_constant());
                emit_op(Code::CALL_ASYNC_CONST, func_idx, expr.start_pos(), 1);
            } else {
                FIBER_ASSERT(constant->constant()->kind == Library::HostCallable::Kind::SyncConstant);
                std::size_t func_idx = add_func_const(constant->constant());
                emit_op(Code::CALL_CONST, func_idx, expr.start_pos(), 1);
            }
            return;
        }
        if (auto *call = dynamic_cast<const ast::FunctionCall *>(&expr)) {
            bool has_spread = false;
            for (const auto &arg: call->args()) {
                if (dynamic_cast<const ast::ExpandArrArg *>(arg.get())) {
                    has_spread = true;
                    break;
                }
            }
            if (!has_spread) {
                for (const auto &arg: call->args()) {
                    if (arg) {
                        compile_expression(*arg);
                    }
                }
                for (const fiber::script::JsValue &default_arg: call->default_args()) {
                    emit_load_js_value(default_arg, expr.start_pos());
                }
                const Library::HostCallable *callable = call->is_async() ? call->async_func() : call->func();
                FIBER_ASSERT(callable->kind == (call->is_async() ? Library::HostCallable::Kind::AsyncFunction
                                                                 : Library::HostCallable::Kind::SyncFunction));
                std::size_t func_idx = add_func_const(callable);
                std::size_t arg_count = call->args().size() + call->default_args().size();
                int delta = 1 - static_cast<int>(arg_count);
                emit_func_call(call->is_async() ? Code::CALL_ASYNC_FUNC : Code::CALL_FUNC, func_idx, arg_count,
                               expr.start_pos(), delta);
                return;
            }
            FIBER_ASSERT(call->default_args().empty());
            emit_raw(static_cast<std::int32_t>(Code::NEW_ARRAY), expr.start_pos(), 1);
            for (const auto &arg: call->args()) {
                if (!arg) {
                    continue;
                }
                if (auto *expand = dynamic_cast<const ast::ExpandArrArg *>(arg.get())) {
                    if (expand->value()) {
                        compile_expression(*expand->value());
                        emit_raw(static_cast<std::int32_t>(Code::EXP_ARRAY), expr.start_pos(), -1);
                    }
                } else {
                    compile_expression(*arg);
                    emit_raw(static_cast<std::int32_t>(Code::PUSH_ARRAY), expr.start_pos(), -1);
                }
            }
            const Library::HostCallable *callable = call->is_async() ? call->async_func() : call->func();
            FIBER_ASSERT(callable->kind == (call->is_async() ? Library::HostCallable::Kind::AsyncFunction
                                                             : Library::HostCallable::Kind::SyncFunction));
            std::size_t func_idx = add_func_const(callable);
            emit_op(call->is_async() ? Code::CALL_ASYNC_FUNC_SPREAD : Code::CALL_FUNC_SPREAD, func_idx,
                    expr.start_pos(), 0);
            return;
        }
        if (auto *list = dynamic_cast<const ast::InlineList *>(&expr)) {
            emit_raw(static_cast<std::int32_t>(Code::NEW_ARRAY), expr.start_pos(), 1);
            for (const auto &item: list->values()) {
                if (!item) {
                    continue;
                }
                if (auto *expand = dynamic_cast<const ast::ExpandArrArg *>(item.get())) {
                    if (expand->value()) {
                        compile_expression(*expand->value());
                        emit_raw(static_cast<std::int32_t>(Code::EXP_ARRAY), expr.start_pos(), -1);
                    }
                } else {
                    compile_expression(*item);
                    emit_raw(static_cast<std::int32_t>(Code::PUSH_ARRAY), expr.start_pos(), -1);
                }
            }
            return;
        }
        if (auto *obj = dynamic_cast<const ast::InlineObject *>(&expr)) {
            emit_raw(static_cast<std::int32_t>(Code::NEW_OBJECT), expr.start_pos(), 1);
            for (const auto &entry: obj->entries()) {
                if (entry.key.kind == ast::InlineObject::KeyKind::Expand) {
                    if (entry.value) {
                        if (auto *expand = dynamic_cast<const ast::ExpandArrArg *>(entry.value.get())) {
                            if (expand->value()) {
                                compile_expression(*expand->value());
                                emit_raw(static_cast<std::int32_t>(Code::EXP_OBJECT), expr.start_pos(), -1);
                            }
                        } else {
                            compile_expression(*entry.value);
                            emit_raw(static_cast<std::int32_t>(Code::EXP_OBJECT), expr.start_pos(), -1);
                        }
                    }
                    continue;
                }
                if (entry.key.kind == ast::InlineObject::KeyKind::Expression) {
                    if (entry.key.expr_key) {
                        compile_expression(*entry.key.expr_key);
                    } else {
                        emit_op(Code::LOAD_CONST, const_undefined(), expr.start_pos(), 1);
                    }
                    if (entry.value) {
                        compile_expression(*entry.value);
                    } else {
                        emit_op(Code::LOAD_CONST, const_undefined(), expr.start_pos(), 1);
                    }
                    emit_raw(static_cast<std::int32_t>(Code::IDX_SET_1), expr.start_pos(), -2);
                    continue;
                }
                std::size_t prop_idx = add_string_constant(entry.key.string_key);
                if (entry.value) {
                    compile_expression(*entry.value);
                } else {
                    emit_op(Code::LOAD_CONST, const_undefined(), expr.start_pos(), 1);
                }
                emit_op(Code::PROP_SET_1, prop_idx, expr.start_pos(), -1);
            }
            return;
        }
        if (auto *indexer = dynamic_cast<const ast::Indexer *>(&expr)) {
            compile_expression(*indexer->parent());
            compile_expression(*indexer->index());
            emit_raw(static_cast<std::int32_t>(Code::IDX_GET), expr.start_pos(), -1);
            return;
        }
        if (auto *prop = dynamic_cast<const ast::PropertyReference *>(&expr)) {
            compile_expression(*prop->parent());
            std::size_t prop_idx = add_string_constant(prop->name());
            emit_op(Code::PROP_GET, prop_idx, expr.start_pos(), 0);
            return;
        }
        if (auto *binary = dynamic_cast<const ast::BinaryOperator *>(&expr)) {
            compile_expression(*binary->left());
            compile_expression(*binary->right());
            std::uint8_t op = Code::BOP_PLUS;
            switch (binary->op()) {
                case ast::Operator::Add:
                    op = Code::BOP_PLUS;
                    break;
                case ast::Operator::Minus:
                    op = Code::BOP_MINUS;
                    break;
                case ast::Operator::Multiply:
                    op = Code::BOP_MULTIPLY;
                    break;
                case ast::Operator::Divide:
                    op = Code::BOP_DIVIDE;
                    break;
                case ast::Operator::Modulo:
                    op = Code::BOP_MOD;
                    break;
                case ast::Operator::Match:
                    op = Code::BOP_MATCH;
                    break;
                case ast::Operator::Lt:
                    op = Code::BOP_LT;
                    break;
                case ast::Operator::Lte:
                    op = Code::BOP_LTE;
                    break;
                case ast::Operator::Gt:
                    op = Code::BOP_GT;
                    break;
                case ast::Operator::Gte:
                    op = Code::BOP_GTE;
                    break;
                case ast::Operator::Eq:
                    op = Code::BOP_EQ;
                    break;
                case ast::Operator::Seq:
                    op = Code::BOP_SEQ;
                    break;
                case ast::Operator::Ne:
                    op = Code::BOP_NE;
                    break;
                case ast::Operator::Sne:
                    op = Code::BOP_SNE;
                    break;
                case ast::Operator::In:
                    op = Code::BOP_IN;
                    break;
                case ast::Operator::And:
                case ast::Operator::Or:
                case ast::Operator::Not:
                case ast::Operator::Typeof:
                    op = Code::BOP_PLUS;
                    break;
            }
            emit_raw(static_cast<std::int32_t>(op), expr.start_pos(), -1);
            return;
        }
        if (auto *logic = dynamic_cast<const ast::LogicRelationalExpression *>(&expr)) {
            compile_expression(*logic->left());
            emit_raw(static_cast<std::int32_t>(Code::DUMP), expr.start_pos(), 1);
            std::size_t end_jump = 0;
            if (logic->op() == ast::Operator::And) {
                end_jump = emit_jump(Code::JUMP_IF_FALSE, 0, expr.start_pos());
            } else {
                end_jump = emit_jump(Code::JUMP_IF_TRUE, 0, expr.start_pos());
            }
            emit_raw(static_cast<std::int32_t>(Code::POP), expr.start_pos(), -1);
            compile_expression(*logic->right());
            std::size_t end_target = codes_.size();
            patch_jump(end_jump, end_target);
            return;
        }
        if (auto *unary = dynamic_cast<const ast::UnaryOperator *>(&expr)) {
            compile_expression(*unary->operand());
            std::uint8_t op = Code::UNARY_PLUS;
            switch (unary->op()) {
                case ast::Operator::Add:
                    op = Code::UNARY_PLUS;
                    break;
                case ast::Operator::Minus:
                    op = Code::UNARY_MINUS;
                    break;
                case ast::Operator::Not:
                    op = Code::UNARY_NEG;
                    break;
                case ast::Operator::Typeof:
                    op = Code::UNARY_TYPEOF;
                    break;
                default:
                    op = Code::UNARY_PLUS;
                    break;
            }
            emit_raw(static_cast<std::int32_t>(op), expr.start_pos(), 0);
            return;
        }
        if (auto *ternary = dynamic_cast<const ast::Ternary *>(&expr)) {
            compile_expression(*ternary->test());
            std::size_t else_jump = emit_jump(Code::JUMP_IF_FALSE, 0, expr.start_pos());
            int saved_depth = stack_depth_;
            compile_expression(*ternary->if_true());
            std::size_t end_jump = emit_jump(Code::JUMP, 0, expr.start_pos());
            std::size_t else_target = codes_.size();
            patch_jump(else_jump, else_target);
            stack_depth_ = saved_depth;
            compile_expression(*ternary->if_false());
            std::size_t end_target = codes_.size();
            patch_jump(end_jump, end_target);
            return;
        }
        if (auto *assign = dynamic_cast<const ast::Assign *>(&expr)) {
            compile_assign(*assign);
            return;
        }
        if (auto *expand = dynamic_cast<const ast::ExpandArrArg *>(&expr)) {
            if (expand->value()) {
                compile_expression(*expand->value());
            }
            return;
        }
    }

    void compile_assign(const ast::Assign &assign) {
        const ast::MaybeLValue *left = assign.left();
        const ast::Expression *right = assign.right();
        if (!left || !right) {
            emit_op(Code::LOAD_CONST, const_undefined(), assign.start_pos(), 1);
            return;
        }
        if (auto *var = dynamic_cast<const ast::VariableReference *>(left)) {
            compile_expression(*right);
            emit_raw(static_cast<std::int32_t>(Code::DUMP), assign.start_pos(), 1);
            std::size_t idx = resolve_var(var->name());
            emit_op(Code::STORE_VAR, idx, assign.start_pos(), -1);
            return;
        }
        if (auto *prop = dynamic_cast<const ast::PropertyReference *>(left)) {
            compile_expression(*prop->parent());
            compile_expression(*right);
            std::size_t prop_idx = add_string_constant(prop->name());
            emit_op(Code::PROP_SET, prop_idx, assign.start_pos(), -1);
            return;
        }
        if (auto *indexer = dynamic_cast<const ast::Indexer *>(left)) {
            compile_expression(*indexer->parent());
            compile_expression(*indexer->index());
            compile_expression(*right);
            emit_raw(static_cast<std::int32_t>(Code::IDX_SET), assign.start_pos(), -2);
            return;
        }
        compile_expression(*right);
    }
};

Compiled Compiler::compile(const ast::Node &node) {
    CompilerImpl compiler;
    return compiler.compile(node);
}

} // namespace fiber::script::ir
