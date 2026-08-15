#ifndef FIBER_SCRIPT_JIT_CFG_H
#define FIBER_SCRIPT_JIT_CFG_H

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include <fiber/script/ir/Compiled.h>

namespace fiber::script::jit {

using BlockId = std::uint32_t;
using ValueId = std::uint32_t;

inline constexpr BlockId kInvalidBlock = UINT32_MAX;
inline constexpr ValueId kInvalidValue = UINT32_MAX;

enum ValueTypeMask : std::uint16_t {
    TypeNone = 0,
    TypeImmediate = 1u << 0u,
    TypeBorrowed = 1u << 1u,
    TypeHeapRef = 1u << 2u,
    TypeAny = TypeImmediate | TypeBorrowed | TypeHeapRef,
};

enum class ValueOrigin : std::uint8_t {
    Undefined,
    Instruction,
    Phi,
};

struct SsaValue {
    ValueId id = kInvalidValue;
    ValueOrigin origin = ValueOrigin::Undefined;
    BlockId block = kInvalidBlock;
    std::uint32_t index = UINT32_MAX;
    std::uint16_t type_mask = TypeAny;
};

enum class EdgeKind : std::uint8_t {
    Normal,
    Exception,
};

struct Edge {
    BlockId predecessor = kInvalidBlock;
    BlockId successor = kInvalidBlock;
    EdgeKind kind = EdgeKind::Normal;
};

enum class PhiKind : std::uint8_t {
    Stack,
    Variable,
    Exception,
};

struct PhiIncoming {
    BlockId predecessor = kInvalidBlock;
    ValueId value = kInvalidValue;
};

struct Phi {
    ValueId result = kInvalidValue;
    PhiKind kind = PhiKind::Variable;
    std::uint32_t slot = 0;
    std::vector<PhiIncoming> incoming;
};

struct Instruction {
    std::uint32_t pc = 0;
    std::uint32_t raw = 0;
    std::uint8_t opcode = 0;
    std::vector<ValueId> operands;
    ValueId result = kInvalidValue;
    ValueId exception = kInvalidValue;
    BlockId normal_target = kInvalidBlock;
    BlockId branch_target = kInvalidBlock;
    BlockId exception_target = kInvalidBlock;
    bool may_abort = false;
    bool is_async = false;
};

struct BasicBlock {
    BlockId id = kInvalidBlock;
    std::uint32_t start_pc = 0;
    std::uint32_t end_pc = 0;
    std::uint32_t input_stack_size = UINT32_MAX;
    std::uint32_t output_stack_size = UINT32_MAX;
    bool reachable = false;

    std::vector<Edge> predecessors;
    std::vector<Edge> successors;
    std::vector<Phi> phis;
    std::vector<Instruction> instructions;

    std::vector<ValueId> entry_stack;
    std::vector<ValueId> entry_variables;
    ValueId entry_exception = kInvalidValue;
    std::vector<ValueId> exit_stack;
    std::vector<ValueId> exit_variables;
    std::vector<ValueId> exception_variables;
};

struct CfgError {
    std::string message;
    std::uint32_t pc = ir::Compiled::kNoPc;
};

class Cfg {
public:
    static std::expected<Cfg, CfgError> build(const ir::Compiled &compiled);

    [[nodiscard]] const ir::Compiled &compiled() const noexcept { return *compiled_; }
    [[nodiscard]] const std::vector<BasicBlock> &blocks() const noexcept { return blocks_; }
    [[nodiscard]] std::vector<BasicBlock> &blocks() noexcept { return blocks_; }
    [[nodiscard]] const std::vector<SsaValue> &values() const noexcept { return values_; }
    [[nodiscard]] ValueId undefined_value() const noexcept { return undefined_value_; }
    [[nodiscard]] BlockId entry_block() const noexcept { return entry_block_; }
    [[nodiscard]] BlockId block_at_pc(std::uint32_t pc) const noexcept;

private:
    friend class CfgBuilder;

    const ir::Compiled *compiled_ = nullptr;
    std::vector<BasicBlock> blocks_;
    std::vector<SsaValue> values_;
    std::vector<BlockId> pc_to_block_;
    ValueId undefined_value_ = kInvalidValue;
    BlockId entry_block_ = kInvalidBlock;
};

class ValueBitSet {
public:
    ValueBitSet() = default;
    explicit ValueBitSet(std::size_t value_count);

    [[nodiscard]] bool contains(ValueId value) const noexcept;
    bool add(ValueId value) noexcept;
    bool remove(ValueId value) noexcept;
    bool union_with(const ValueBitSet &other) noexcept;
    void subtract(const ValueBitSet &other) noexcept;
    [[nodiscard]] std::vector<ValueId> values() const;

private:
    std::vector<std::uint64_t> words_;
};

struct BlockLiveness {
    ValueBitSet live_in;
    ValueBitSet live_out;
    std::vector<ValueBitSet> live_before;
    std::vector<ValueBitSet> live_after;
};

class Liveness {
public:
    static Liveness analyze(const Cfg &cfg);

    [[nodiscard]] const BlockLiveness &block(BlockId id) const noexcept { return blocks_[id]; }

private:
    std::vector<BlockLiveness> blocks_;
};

struct AsyncSiteSpill {
    BlockId block = kInvalidBlock;
    std::uint32_t instruction_index = UINT32_MAX;
    std::uint32_t resume_id = 0;
    std::vector<ValueId> values;
};

class AsyncSpill {
public:
    static AsyncSpill analyze(const Cfg &cfg, const Liveness &liveness);

    [[nodiscard]] const std::vector<AsyncSiteSpill> &sites() const noexcept { return sites_; }
    [[nodiscard]] const std::vector<ValueId> &persistent_values() const noexcept { return persistent_values_; }
    [[nodiscard]] const AsyncSiteSpill *site(BlockId block, std::uint32_t instruction_index) const noexcept;

private:
    std::vector<AsyncSiteSpill> sites_;
    std::vector<ValueId> persistent_values_;
};

[[nodiscard]] bool opcode_is_async(std::uint8_t opcode) noexcept;
[[nodiscard]] bool opcode_may_exception(std::uint8_t opcode) noexcept;
[[nodiscard]] bool opcode_may_abort(std::uint8_t opcode) noexcept;

} // namespace fiber::script::jit

#endif // FIBER_SCRIPT_JIT_CFG_H
