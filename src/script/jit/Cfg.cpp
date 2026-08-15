#include <fiber/script/jit/Cfg.h>

#include <algorithm>
#include <bit>
#include <deque>
#include <limits>
#include <ranges>
#include <utility>

#include <fiber/script/JsValue.h>

namespace fiber::script::jit {

namespace {

constexpr std::uint32_t kInstrumentLen = 8;
constexpr std::uint32_t kIteratorLen = 12;
constexpr std::uint32_t kIteratorOff = kInstrumentLen + kIteratorLen;
constexpr std::uint32_t kMaxIteratorVar = (1u << kIteratorLen) - 1u;

bool is_binary(std::uint8_t op) noexcept { return op >= ir::Code::BOP_PLUS && op <= ir::Code::BOP_IN; }

bool is_unary(std::uint8_t op) noexcept { return op >= ir::Code::UNARY_PLUS && op <= ir::Code::UNARY_TYPEOF; }

bool is_call(std::uint8_t op) noexcept {
    return op == ir::Code::CALL_FUNC || op == ir::Code::CALL_FUNC_SPREAD || op == ir::Code::CALL_ASYNC_FUNC ||
           op == ir::Code::CALL_ASYNC_FUNC_SPREAD || op == ir::Code::CALL_CONST || op == ir::Code::CALL_ASYNC_CONST;
}

bool is_runtime_operation(std::uint8_t op) noexcept {
    return is_binary(op) || is_unary(op) || (op >= ir::Code::NEW_OBJECT && op <= ir::Code::PROP_SET_1) || is_call(op) ||
           op == ir::Code::ITERATE_INTO || op == ir::Code::ITERATE_NEXT;
}

bool is_terminal(std::uint8_t op) noexcept { return op == ir::Code::THROW_EXP || op == ir::Code::END_RETURN; }

bool is_jump(std::uint8_t op) noexcept {
    return op == ir::Code::JUMP || op == ir::Code::JUMP_IF_FALSE || op == ir::Code::JUMP_IF_TRUE;
}

bool is_conditional(std::uint8_t op) noexcept { return op == ir::Code::JUMP_IF_FALSE || op == ir::Code::JUMP_IF_TRUE; }

std::uint32_t encoded_argc(std::uint32_t raw, std::uint8_t op) noexcept {
    if (op == ir::Code::CALL_FUNC || op == ir::Code::CALL_ASYNC_FUNC) {
        return (raw >> 8u) & 0xFFu;
    }
    return 0;
}

bool uses_spread(std::uint8_t op) noexcept {
    return op == ir::Code::CALL_FUNC_SPREAD || op == ir::Code::CALL_ASYNC_FUNC_SPREAD;
}

struct StackEffect {
    std::uint32_t required = 0;
    std::int32_t delta = 0;
};

StackEffect stack_effect(std::uint32_t raw, std::uint8_t op) noexcept {
    if (op == ir::Code::LOAD_CONST || op == ir::Code::LOAD_ROOT || op == ir::Code::LOAD_VAR ||
        op == ir::Code::NEW_OBJECT || op == ir::Code::NEW_ARRAY || op == ir::Code::CALL_CONST ||
        op == ir::Code::CALL_ASYNC_CONST || op == ir::Code::ITERATE_NEXT) {
        return {0, 1};
    }
    if (op == ir::Code::DUMP) {
        return {1, 1};
    }
    if (op == ir::Code::POP || op == ir::Code::STORE_VAR || op == ir::Code::JUMP_IF_FALSE ||
        op == ir::Code::JUMP_IF_TRUE || op == ir::Code::ITERATE_INTO || op == ir::Code::THROW_EXP) {
        return {1, -1};
    }
    if (is_binary(op) || op == ir::Code::EXP_OBJECT || op == ir::Code::EXP_ARRAY || op == ir::Code::PUSH_ARRAY ||
        op == ir::Code::IDX_GET || op == ir::Code::PROP_SET || op == ir::Code::PROP_SET_1) {
        return {2, -1};
    }
    if (op == ir::Code::IDX_SET || op == ir::Code::IDX_SET_1) {
        return {3, -2};
    }
    if (is_unary(op) || op == ir::Code::PROP_GET || uses_spread(op)) {
        return {1, 0};
    }
    if (op == ir::Code::CALL_FUNC || op == ir::Code::CALL_ASYNC_FUNC) {
        std::uint32_t argc = encoded_argc(raw, op);
        return {argc, 1 - static_cast<std::int32_t>(argc)};
    }
    return {};
}

std::uint16_t constant_type_mask(const JsValue &value) noexcept {
    switch (js_value_tag(value)) {
        case JsTag::HeapRef:
            return TypeHeapRef;
        case JsTag::BorrowedString:
        case JsTag::BorrowedBinary:
            return TypeBorrowed;
        case JsTag::Undefined:
        case JsTag::Null:
        case JsTag::Boolean:
        case JsTag::Int64:
        case JsTag::Double:
        case JsTag::Exception:
            return TypeImmediate;
    }
    return TypeAny;
}

std::uint16_t result_type_mask(std::uint8_t op) noexcept {
    if (op == ir::Code::NEW_OBJECT || op == ir::Code::NEW_ARRAY || op == ir::Code::ITERATE_INTO) {
        return TypeHeapRef;
    }
    if ((op >= ir::Code::BOP_LT && op <= ir::Code::BOP_IN) || op == ir::Code::ITERATE_NEXT) {
        return TypeImmediate;
    }
    return TypeAny;
}

} // namespace

class CfgBuilder {
public:
    explicit CfgBuilder(const ir::Compiled &compiled) : compiled_(compiled) {
        cfg_.compiled_ = &compiled_;
        cfg_.pc_to_block_.assign(compiled_.code_size(), kInvalidBlock);
    }

    std::expected<Cfg, CfgError> build() {
        if (compiled_.code_size() == 0) {
            return std::unexpected(CfgError{"empty bytecode", 0});
        }
        if (!discover_blocks() || !connect_blocks() || !propagate_stack_sizes() || !create_entry_values() ||
            !simulate_blocks() || !fill_phi_inputs() || !infer_phi_types()) {
            return std::unexpected(std::move(error_));
        }
        return std::move(cfg_);
    }

private:
    const ir::Compiled &compiled_;
    Cfg cfg_;
    CfgError error_;

    bool fail(const char *message, std::uint32_t pc) {
        error_.message = message;
        error_.pc = pc;
        return false;
    }

    ValueId add_value(ValueOrigin origin, BlockId block, std::uint32_t index, std::uint16_t mask) {
        ValueId id = static_cast<ValueId>(cfg_.values_.size());
        cfg_.values_.push_back(SsaValue{id, origin, block, index, mask});
        return id;
    }

    BlockId block_for_pc(std::uint32_t pc) const noexcept {
        return pc < cfg_.pc_to_block_.size() ? cfg_.pc_to_block_[pc] : kInvalidBlock;
    }

    bool discover_blocks() {
        std::uint32_t code_size = compiled_.code_size();
        std::vector<bool> leaders(code_size + 1, false);
        leaders[0] = true;
        const std::int32_t *codes = compiled_.codes();
        for (std::uint32_t pc = 0; pc < code_size; ++pc) {
            std::uint32_t raw = static_cast<std::uint32_t>(codes[pc]);
            std::uint8_t op = static_cast<std::uint8_t>(raw & 0xFFu);
            if (is_jump(op)) {
                std::uint32_t target = raw >> 8u;
                if (target >= code_size) {
                    return fail("jump target out of range", pc);
                }
                leaders[target] = true;
            }
            std::uint32_t catch_pc = compiled_.find_catch(pc);
            if (catch_pc != ir::Compiled::kNoPc) {
                if (catch_pc >= code_size) {
                    return fail("catch target out of range", pc);
                }
                leaders[catch_pc] = true;
            }
            if (pc + 1 < code_size && (is_jump(op) || is_terminal(op) || is_runtime_operation(op))) {
                leaders[pc + 1] = true;
            }
        }

        for (std::uint32_t start = 0; start < code_size;) {
            std::uint32_t end = start + 1;
            while (end < code_size && !leaders[end]) {
                ++end;
            }
            BlockId id = static_cast<BlockId>(cfg_.blocks_.size());
            BasicBlock block;
            block.id = id;
            block.start_pc = start;
            block.end_pc = end;
            cfg_.blocks_.push_back(std::move(block));
            for (std::uint32_t pc = start; pc < end; ++pc) {
                cfg_.pc_to_block_[pc] = id;
            }
            start = end;
        }
        cfg_.entry_block_ = 0;
        cfg_.undefined_value_ = add_value(ValueOrigin::Undefined, kInvalidBlock, 0, TypeImmediate);
        return true;
    }

    bool add_edge(BlockId from, BlockId to, EdgeKind kind, std::uint32_t pc) {
        if (from >= cfg_.blocks_.size() || to >= cfg_.blocks_.size()) {
            return fail("CFG edge target out of range", pc);
        }
        Edge edge{from, to, kind};
        cfg_.blocks_[from].successors.push_back(edge);
        cfg_.blocks_[to].predecessors.push_back(edge);
        return true;
    }

    bool connect_blocks() {
        const std::int32_t *codes = compiled_.codes();
        for (BasicBlock &block: cfg_.blocks_) {
            std::uint32_t pc = block.end_pc - 1;
            std::uint32_t raw = static_cast<std::uint32_t>(codes[pc]);
            std::uint8_t op = static_cast<std::uint8_t>(raw & 0xFFu);
            if (op == ir::Code::JUMP) {
                if (!add_edge(block.id, block_for_pc(raw >> 8u), EdgeKind::Normal, pc)) {
                    return false;
                }
            } else if (is_conditional(op)) {
                if (!add_edge(block.id, block_for_pc(raw >> 8u), EdgeKind::Normal, pc)) {
                    return false;
                }
                if (block.end_pc < compiled_.code_size() &&
                    !add_edge(block.id, block_for_pc(block.end_pc), EdgeKind::Normal, pc)) {
                    return false;
                }
            } else if (!is_terminal(op) && block.end_pc < compiled_.code_size()) {
                if (!add_edge(block.id, block_for_pc(block.end_pc), EdgeKind::Normal, pc)) {
                    return false;
                }
            }
            if (opcode_may_exception(op)) {
                std::uint32_t catch_pc = compiled_.find_catch(pc);
                if (catch_pc != ir::Compiled::kNoPc &&
                    !add_edge(block.id, block_for_pc(catch_pc), EdgeKind::Exception, pc)) {
                    return false;
                }
            }
        }
        return true;
    }

    bool compute_output_height(BasicBlock &block) {
        std::int64_t height = block.input_stack_size;
        const std::int32_t *codes = compiled_.codes();
        for (std::uint32_t pc = block.start_pc; pc < block.end_pc; ++pc) {
            std::uint32_t raw = static_cast<std::uint32_t>(codes[pc]);
            std::uint8_t op = static_cast<std::uint8_t>(raw & 0xFFu);
            StackEffect effect = stack_effect(raw, op);
            if (height < effect.required) {
                return fail("stack underflow", pc);
            }
            height += effect.delta;
            if (height < 0 || static_cast<std::uint64_t>(height) > compiled_.stack_size()) {
                return fail("stack size out of range", pc);
            }
            if (op == ir::Code::END_RETURN && height > 1) {
                return fail("invalid return stack size", pc);
            }
        }
        block.output_stack_size = static_cast<std::uint32_t>(height);
        return true;
    }

    bool propagate_stack_sizes() {
        BasicBlock &entry = cfg_.blocks_[cfg_.entry_block_];
        entry.input_stack_size = 0;
        entry.reachable = true;
        std::deque<BlockId> work;
        work.push_back(entry.id);
        while (!work.empty()) {
            BlockId id = work.front();
            work.pop_front();
            BasicBlock &block = cfg_.blocks_[id];
            if (!compute_output_height(block)) {
                return false;
            }
            for (const Edge &edge: block.successors) {
                BasicBlock &successor = cfg_.blocks_[edge.successor];
                std::uint32_t incoming = edge.kind == EdgeKind::Exception ? 0 : block.output_stack_size;
                if (successor.input_stack_size == UINT32_MAX) {
                    successor.input_stack_size = incoming;
                    successor.reachable = true;
                    work.push_back(successor.id);
                } else if (successor.input_stack_size != incoming) {
                    return fail("inconsistent stack size at merge", successor.start_pc);
                }
            }
        }
        return true;
    }

    ValueId add_phi(BasicBlock &block, PhiKind kind, std::uint32_t slot) {
        std::uint32_t index = static_cast<std::uint32_t>(block.phis.size());
        ValueId result = add_value(ValueOrigin::Phi, block.id, index, TypeNone);
        block.phis.push_back(Phi{result, kind, slot, {}});
        return result;
    }

    bool create_entry_values() {
        for (BasicBlock &block: cfg_.blocks_) {
            if (!block.reachable) {
                continue;
            }
            block.entry_stack.resize(block.input_stack_size, kInvalidValue);
            block.entry_variables.resize(compiled_.var_table_size(), cfg_.undefined_value_);
            if (block.id == cfg_.entry_block_) {
                continue;
            }
            for (std::uint32_t i = 0; i < block.input_stack_size; ++i) {
                block.entry_stack[i] = add_phi(block, PhiKind::Stack, i);
            }
            for (std::uint32_t i = 0; i < compiled_.var_table_size(); ++i) {
                block.entry_variables[i] = add_phi(block, PhiKind::Variable, i);
            }
            bool has_exception_pred = std::ranges::any_of(
                    block.predecessors, [](const Edge &edge) { return edge.kind == EdgeKind::Exception; });
            if (has_exception_pred) {
                block.entry_exception = add_phi(block, PhiKind::Exception, 0);
            }
        }
        return true;
    }

    ValueId add_instruction_value(BasicBlock &block, std::uint32_t instruction_index, std::uint16_t mask) {
        return add_value(ValueOrigin::Instruction, block.id, instruction_index, mask);
    }

    bool valid_var(std::uint32_t index, std::uint32_t pc) {
        return index < compiled_.var_table_size() || fail("variable index out of range", pc);
    }

    bool pop(std::vector<ValueId> &stack, ValueId &value, std::uint32_t pc) {
        if (stack.empty()) {
            return fail("stack underflow during SSA construction", pc);
        }
        value = stack.back();
        stack.pop_back();
        return true;
    }

    bool simulate_blocks() {
        const std::int32_t *codes = compiled_.codes();
        for (BasicBlock &block: cfg_.blocks_) {
            if (!block.reachable) {
                continue;
            }
            std::vector<ValueId> stack = block.entry_stack;
            std::vector<ValueId> vars = block.entry_variables;
            for (std::uint32_t pc = block.start_pc; pc < block.end_pc; ++pc) {
                std::uint32_t raw = static_cast<std::uint32_t>(codes[pc]);
                std::uint8_t op = static_cast<std::uint8_t>(raw & 0xFFu);
                Instruction instruction;
                instruction.pc = pc;
                instruction.raw = raw;
                instruction.opcode = op;
                instruction.may_abort = opcode_may_abort(op);
                instruction.is_async = opcode_is_async(op);
                if (opcode_may_exception(op)) {
                    block.exception_variables = vars;
                }
                std::uint32_t instruction_index = static_cast<std::uint32_t>(block.instructions.size());
                auto make_result = [&](std::uint16_t mask = TypeAny) {
                    instruction.result = add_instruction_value(block, instruction_index, mask);
                    stack.push_back(instruction.result);
                };
                auto add_exception = [&] {
                    if (opcode_may_exception(op)) {
                        instruction.exception = add_instruction_value(block, instruction_index, TypeAny);
                        std::uint32_t catch_pc = compiled_.find_catch(pc);
                        instruction.exception_target =
                                catch_pc == ir::Compiled::kNoPc ? kInvalidBlock : block_for_pc(catch_pc);
                    }
                };
                ValueId a = kInvalidValue;
                ValueId b = kInvalidValue;
                ValueId c = kInvalidValue;
                bool emit = true;
                if (op == ir::Code::NOOP) {
                    emit = false;
                } else if (op == ir::Code::LOAD_CONST) {
                    std::uint32_t index = raw >> 8u;
                    make_result(constant_type_mask(compiled_.constant(index)));
                } else if (op == ir::Code::LOAD_ROOT) {
                    make_result(TypeAny);
                } else if (op == ir::Code::DUMP) {
                    if (stack.empty()) {
                        return fail("stack underflow at dump", pc);
                    }
                    stack.push_back(stack.back());
                    emit = false;
                } else if (op == ir::Code::POP) {
                    if (!pop(stack, a, pc)) {
                        return false;
                    }
                    emit = false;
                } else if (op == ir::Code::LOAD_VAR) {
                    std::uint32_t index = raw >> 8u;
                    if (!valid_var(index, pc)) {
                        return false;
                    }
                    stack.push_back(vars[index]);
                    emit = false;
                } else if (op == ir::Code::STORE_VAR) {
                    std::uint32_t index = raw >> 8u;
                    if (!valid_var(index, pc) || !pop(stack, vars[index], pc)) {
                        return false;
                    }
                    emit = false;
                } else if (is_binary(op) || op == ir::Code::EXP_OBJECT || op == ir::Code::EXP_ARRAY ||
                           op == ir::Code::PUSH_ARRAY || op == ir::Code::IDX_GET || op == ir::Code::PROP_SET ||
                           op == ir::Code::PROP_SET_1) {
                    if (!pop(stack, b, pc) || !pop(stack, a, pc)) {
                        return false;
                    }
                    instruction.operands = {a, b};
                    add_exception();
                    make_result(result_type_mask(op));
                } else if (op == ir::Code::IDX_SET || op == ir::Code::IDX_SET_1) {
                    if (!pop(stack, c, pc) || !pop(stack, b, pc) || !pop(stack, a, pc)) {
                        return false;
                    }
                    instruction.operands = {a, b, c};
                    add_exception();
                    make_result(TypeAny);
                } else if (is_unary(op) || op == ir::Code::PROP_GET) {
                    if (!pop(stack, a, pc)) {
                        return false;
                    }
                    instruction.operands = {a};
                    add_exception();
                    make_result(result_type_mask(op));
                } else if (op == ir::Code::NEW_OBJECT || op == ir::Code::NEW_ARRAY) {
                    make_result(TypeHeapRef);
                } else if (is_call(op)) {
                    std::uint32_t argc = encoded_argc(raw, op);
                    if (uses_spread(op)) {
                        if (!pop(stack, a, pc)) {
                            return false;
                        }
                        instruction.operands.push_back(a);
                    } else {
                        if (stack.size() < argc) {
                            return fail("stack underflow at call", pc);
                        }
                        std::size_t base = stack.size() - argc;
                        instruction.operands.insert(instruction.operands.end(), stack.begin() + base, stack.end());
                        stack.resize(base);
                    }
                    add_exception();
                    make_result(TypeAny);
                } else if (op == ir::Code::JUMP_IF_FALSE || op == ir::Code::JUMP_IF_TRUE) {
                    if (!pop(stack, a, pc)) {
                        return false;
                    }
                    instruction.operands = {a};
                    instruction.branch_target = block_for_pc(raw >> 8u);
                } else if (op == ir::Code::JUMP) {
                    instruction.branch_target = block_for_pc(raw >> 8u);
                } else if (op == ir::Code::ITERATE_INTO) {
                    std::uint32_t index = raw >> kInstrumentLen;
                    if (!valid_var(index, pc) || !pop(stack, a, pc)) {
                        return false;
                    }
                    instruction.operands = {a};
                    add_exception();
                    instruction.result = add_instruction_value(block, instruction_index, TypeHeapRef);
                    vars[index] = instruction.result;
                } else if (op == ir::Code::ITERATE_NEXT) {
                    std::uint32_t index = raw >> kInstrumentLen;
                    if (!valid_var(index, pc)) {
                        return false;
                    }
                    instruction.operands = {vars[index]};
                    add_exception();
                    make_result(TypeImmediate);
                } else if (op == ir::Code::ITERATE_KEY || op == ir::Code::ITERATE_VALUE) {
                    std::uint32_t var_index = (raw >> kInstrumentLen) & kMaxIteratorVar;
                    std::uint32_t iter_index = raw >> kIteratorOff;
                    if (!valid_var(var_index, pc) || !valid_var(iter_index, pc)) {
                        return false;
                    }
                    instruction.operands = {vars[iter_index]};
                    instruction.result = add_instruction_value(block, instruction_index, TypeAny);
                    vars[var_index] = instruction.result;
                } else if (op == ir::Code::INTO_CATCH) {
                    std::uint32_t index = raw >> kInstrumentLen;
                    if (!valid_var(index, pc) || block.entry_exception == kInvalidValue) {
                        return fail("catch entry has no exception value", pc);
                    }
                    vars[index] = block.entry_exception;
                    emit = false;
                } else if (op == ir::Code::THROW_EXP) {
                    if (!pop(stack, a, pc)) {
                        return false;
                    }
                    instruction.operands = {a};
                    instruction.exception = a;
                    std::uint32_t catch_pc = compiled_.find_catch(pc);
                    instruction.exception_target =
                            catch_pc == ir::Compiled::kNoPc ? kInvalidBlock : block_for_pc(catch_pc);
                } else if (op == ir::Code::END_RETURN) {
                    if (stack.size() > 1) {
                        return fail("invalid return stack size", pc);
                    }
                    if (!stack.empty()) {
                        instruction.operands.push_back(stack.back());
                    }
                } else {
                    return fail("invalid opcode", pc);
                }

                if (emit) {
                    if (!is_terminal(op) && !is_jump(op) && pc + 1 < compiled_.code_size()) {
                        instruction.normal_target = block_for_pc(pc + 1);
                    }
                    block.instructions.push_back(std::move(instruction));
                }
            }
            block.exit_stack = std::move(stack);
            block.exit_variables = std::move(vars);
        }
        return true;
    }

    const Edge *find_edge(BlockId predecessor, BlockId successor, EdgeKind kind) const noexcept {
        const BasicBlock &block = cfg_.blocks_[predecessor];
        auto it = std::ranges::find_if(
                block.successors, [&](const Edge &edge) { return edge.successor == successor && edge.kind == kind; });
        return it == block.successors.end() ? nullptr : &*it;
    }

    ValueId exceptional_value(const BasicBlock &predecessor) const noexcept {
        if (predecessor.instructions.empty()) {
            return kInvalidValue;
        }
        return predecessor.instructions.back().exception;
    }

    bool fill_phi_inputs() {
        for (BasicBlock &block: cfg_.blocks_) {
            if (!block.reachable || block.id == cfg_.entry_block_) {
                continue;
            }
            for (Phi &phi: block.phis) {
                for (const Edge &edge: block.predecessors) {
                    if (!cfg_.blocks_[edge.predecessor].reachable) {
                        continue;
                    }
                    const BasicBlock &predecessor = cfg_.blocks_[edge.predecessor];
                    ValueId value = kInvalidValue;
                    if (phi.kind == PhiKind::Exception) {
                        if (edge.kind != EdgeKind::Exception) {
                            continue;
                        }
                        value = exceptional_value(predecessor);
                    } else if (phi.kind == PhiKind::Stack) {
                        if (edge.kind == EdgeKind::Exception || phi.slot >= predecessor.exit_stack.size()) {
                            continue;
                        }
                        value = predecessor.exit_stack[phi.slot];
                    } else {
                        const std::vector<ValueId> &variables =
                                edge.kind == EdgeKind::Exception && !predecessor.exception_variables.empty()
                                        ? predecessor.exception_variables
                                        : predecessor.exit_variables;
                        if (phi.slot >= variables.size()) {
                            return fail("missing variable at CFG edge", block.start_pc);
                        }
                        value = variables[phi.slot];
                    }
                    if (value == kInvalidValue) {
                        return fail("missing SSA value at CFG edge", block.start_pc);
                    }
                    phi.incoming.push_back(PhiIncoming{edge.predecessor, value});
                }
                if (phi.incoming.empty()) {
                    return fail("phi has no incoming values", block.start_pc);
                }
            }
        }
        return true;
    }

    bool infer_phi_types() {
        bool changed = true;
        std::size_t rounds = 0;
        while (changed && rounds++ <= cfg_.values_.size()) {
            changed = false;
            for (const BasicBlock &block: cfg_.blocks_) {
                for (const Phi &phi: block.phis) {
                    std::uint16_t mask = TypeNone;
                    for (const PhiIncoming &incoming: phi.incoming) {
                        mask = static_cast<std::uint16_t>(mask | cfg_.values_[incoming.value].type_mask);
                    }
                    SsaValue &value = cfg_.values_[phi.result];
                    if (value.type_mask != mask) {
                        value.type_mask = mask;
                        changed = true;
                    }
                }
            }
        }
        for (SsaValue &value: cfg_.values_) {
            if (value.type_mask == TypeNone) {
                value.type_mask = TypeAny;
            }
        }
        return true;
    }
};

namespace {

ValueBitSet edge_live_in(const Cfg &cfg, const std::vector<BlockLiveness> &liveness, const Edge &edge) {
    const BasicBlock &successor = cfg.blocks()[edge.successor];
    ValueBitSet result = liveness[edge.successor].live_out;
    for (std::size_t i = successor.instructions.size(); i > 0; --i) {
        const Instruction &instruction = successor.instructions[i - 1];
        if (instruction.result != kInvalidValue) {
            result.remove(instruction.result);
        }
        if (instruction.exception != kInvalidValue && instruction.exception != instruction.result) {
            result.remove(instruction.exception);
        }
        for (ValueId operand: instruction.operands) {
            result.add(operand);
        }
    }
    for (const Phi &phi: successor.phis) {
        bool phi_is_live = result.contains(phi.result);
        result.remove(phi.result);
        if (!phi_is_live) {
            continue;
        }
        auto incoming = std::ranges::find_if(
                phi.incoming, [&](const PhiIncoming &item) { return item.predecessor == edge.predecessor; });
        if (incoming != phi.incoming.end()) {
            result.add(incoming->value);
        }
    }
    return result;
}

} // namespace

bool opcode_is_async(std::uint8_t opcode) noexcept {
    return opcode == ir::Code::CALL_ASYNC_FUNC || opcode == ir::Code::CALL_ASYNC_FUNC_SPREAD ||
           opcode == ir::Code::CALL_ASYNC_CONST;
}

bool opcode_may_exception(std::uint8_t opcode) noexcept {
    return is_binary(opcode) || is_unary(opcode) ||
           (opcode >= ir::Code::EXP_OBJECT && opcode <= ir::Code::PROP_SET_1) || is_call(opcode) ||
           opcode == ir::Code::ITERATE_INTO || opcode == ir::Code::ITERATE_NEXT || opcode == ir::Code::THROW_EXP;
}

bool opcode_may_abort(std::uint8_t opcode) noexcept {
    return is_runtime_operation(opcode) || opcode == ir::Code::THROW_EXP;
}

BlockId Cfg::block_at_pc(std::uint32_t pc) const noexcept {
    return pc < pc_to_block_.size() ? pc_to_block_[pc] : kInvalidBlock;
}

std::expected<Cfg, CfgError> Cfg::build(const ir::Compiled &compiled) { return CfgBuilder(compiled).build(); }

ValueBitSet::ValueBitSet(std::size_t value_count) : words_((value_count + 63u) / 64u, 0) {}

bool ValueBitSet::contains(ValueId value) const noexcept {
    std::size_t word = value / 64u;
    return word < words_.size() && (words_[word] & (std::uint64_t{1} << (value % 64u))) != 0;
}

bool ValueBitSet::add(ValueId value) noexcept {
    std::size_t word = value / 64u;
    if (word >= words_.size()) {
        return false;
    }
    std::uint64_t mask = std::uint64_t{1} << (value % 64u);
    bool changed = (words_[word] & mask) == 0;
    words_[word] |= mask;
    return changed;
}

bool ValueBitSet::remove(ValueId value) noexcept {
    std::size_t word = value / 64u;
    if (word >= words_.size()) {
        return false;
    }
    std::uint64_t mask = std::uint64_t{1} << (value % 64u);
    bool changed = (words_[word] & mask) != 0;
    words_[word] &= ~mask;
    return changed;
}

bool ValueBitSet::union_with(const ValueBitSet &other) noexcept {
    bool changed = false;
    std::size_t count = std::min(words_.size(), other.words_.size());
    for (std::size_t i = 0; i < count; ++i) {
        std::uint64_t value = words_[i] | other.words_[i];
        changed |= value != words_[i];
        words_[i] = value;
    }
    return changed;
}

void ValueBitSet::subtract(const ValueBitSet &other) noexcept {
    std::size_t count = std::min(words_.size(), other.words_.size());
    for (std::size_t i = 0; i < count; ++i) {
        words_[i] &= ~other.words_[i];
    }
}

std::vector<ValueId> ValueBitSet::values() const {
    std::vector<ValueId> result;
    for (std::size_t word = 0; word < words_.size(); ++word) {
        std::uint64_t bits = words_[word];
        while (bits != 0) {
            unsigned bit = std::countr_zero(bits);
            result.push_back(static_cast<ValueId>(word * 64u + bit));
            bits &= bits - 1u;
        }
    }
    return result;
}

Liveness Liveness::analyze(const Cfg &cfg) {
    Liveness result;
    std::size_t value_count = cfg.values().size();
    result.blocks_.resize(cfg.blocks().size());
    std::vector<ValueBitSet> uses;
    std::vector<ValueBitSet> defs;
    uses.reserve(cfg.blocks().size());
    defs.reserve(cfg.blocks().size());
    for (const BasicBlock &block: cfg.blocks()) {
        uses.emplace_back(value_count);
        defs.emplace_back(value_count);
        result.blocks_[block.id].live_in = ValueBitSet(value_count);
        result.blocks_[block.id].live_out = ValueBitSet(value_count);
        for (const Phi &phi: block.phis) {
            defs.back().add(phi.result);
        }
        for (const Instruction &instruction: block.instructions) {
            for (ValueId operand: instruction.operands) {
                if (!defs.back().contains(operand)) {
                    uses.back().add(operand);
                }
            }
            if (instruction.result != kInvalidValue) {
                defs.back().add(instruction.result);
            }
            if (instruction.exception != kInvalidValue && instruction.exception != instruction.result) {
                defs.back().add(instruction.exception);
            }
        }
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t reverse = cfg.blocks().size(); reverse > 0; --reverse) {
            const BasicBlock &block = cfg.blocks()[reverse - 1];
            if (!block.reachable) {
                continue;
            }
            ValueBitSet out(value_count);
            for (const Edge &edge: block.successors) {
                out.union_with(edge_live_in(cfg, result.blocks_, edge));
            }
            ValueBitSet in = out;
            in.subtract(defs[block.id]);
            in.union_with(uses[block.id]);
            changed |= result.blocks_[block.id].live_out.union_with(out);
            changed |= result.blocks_[block.id].live_in.union_with(in);
        }
    }

    for (const BasicBlock &block: cfg.blocks()) {
        BlockLiveness &block_result = result.blocks_[block.id];
        block_result.live_before.resize(block.instructions.size(), ValueBitSet(value_count));
        block_result.live_after.resize(block.instructions.size(), ValueBitSet(value_count));
        ValueBitSet live = block_result.live_out;
        for (std::size_t i = block.instructions.size(); i > 0; --i) {
            const Instruction &instruction = block.instructions[i - 1];
            block_result.live_after[i - 1] = live;
            if (instruction.result != kInvalidValue) {
                live.remove(instruction.result);
            }
            if (instruction.exception != kInvalidValue && instruction.exception != instruction.result) {
                live.remove(instruction.exception);
            }
            for (ValueId operand: instruction.operands) {
                live.add(operand);
            }
            block_result.live_before[i - 1] = live;
        }
    }
    return result;
}

AsyncSpill AsyncSpill::analyze(const Cfg &cfg, const Liveness &liveness) {
    AsyncSpill result;
    ValueBitSet all(cfg.values().size());
    std::uint32_t resume_id = 1;
    for (const BasicBlock &block: cfg.blocks()) {
        for (std::uint32_t i = 0; i < block.instructions.size(); ++i) {
            const Instruction &instruction = block.instructions[i];
            if (!instruction.is_async) {
                continue;
            }
            ValueBitSet spill = liveness.block(block.id).live_after[i];
            if (instruction.result != kInvalidValue) {
                spill.remove(instruction.result);
            }
            if (instruction.exception != kInvalidValue) {
                spill.remove(instruction.exception);
            }
            AsyncSiteSpill site;
            site.block = block.id;
            site.instruction_index = i;
            site.resume_id = resume_id++;
            site.values = spill.values();
            for (ValueId value: site.values) {
                all.add(value);
            }
            result.sites_.push_back(std::move(site));
        }
    }
    result.persistent_values_ = all.values();
    return result;
}

const AsyncSiteSpill *AsyncSpill::site(BlockId block, std::uint32_t instruction_index) const noexcept {
    auto it = std::ranges::find_if(sites_, [&](const AsyncSiteSpill &site) {
        return site.block == block && site.instruction_index == instruction_index;
    });
    return it == sites_.end() ? nullptr : &*it;
}

} // namespace fiber::script::jit
