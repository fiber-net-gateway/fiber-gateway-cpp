#if FIBER_ENABLE_SCRIPT_JIT

#include <fiber/script/jit/JitCompiler.h>

#include "StackMapPlugin.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <llvm/ADT/APInt.h>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ExecutionEngine/Orc/AbsoluteSymbols.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/Mangling.h>
#include <llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/TargetSelect.h>

#include <fiber/script/JsValue.h>
#include <fiber/script/gc/GcInternal.h>
#include <fiber/script/ir/Code.h>
#include <fiber/script/jit/Cfg.h>
#include <fiber/script/run/JitCode.h>
#include <fiber/script/run/JitRuntime.h>

namespace fiber::script::jit {

namespace {

using llvm::Function;
using llvm::IRBuilder;
using llvm::Type;
using llvm::Value;

static_assert(sizeof(JsValue) == 16);
static_assert(alignof(JsValue) == 16);
static_assert(offsetof(JsValue, payload) == 0);
static_assert(offsetof(JsValue, tag) == 14);
static_assert(sizeof(std::size_t) == sizeof(std::uint64_t));
static_assert(offsetof(GcIterator, current_key) % alignof(JsValue) == 0);
static_assert(offsetof(GcIterator, current_value) % alignof(JsValue) == 0);

constexpr std::uint32_t kInvalidSlot = UINT32_MAX;
constexpr std::size_t kMaxJitCodeCount = 1u << 20u;
constexpr std::size_t kMaxJitBlockCount = 1u << 18u;
constexpr std::size_t kMaxJitEdgeCount = 1u << 20u;
constexpr std::size_t kMaxJitValueCount = 1u << 20u;
constexpr std::size_t kMaxJitAsyncSlots = 1u << 16u;
constexpr std::size_t kMaxStatepointRoots = 4096u;
constexpr std::size_t kBinaryOpcodeCount = ir::Code::BOP_IN - ir::Code::BOP_PLUS + 1u;
constexpr std::size_t kUnaryOpcodeCount = ir::Code::UNARY_TYPEOF - ir::Code::UNARY_PLUS + 1u;

constexpr std::array<const char *, kBinaryOpcodeCount> kBinaryHelperNames{
        "fiber_script_jit_bop_plus",   "fiber_script_jit_bop_minus",  "fiber_script_jit_bop_multiply",
        "fiber_script_jit_bop_divide", "fiber_script_jit_bop_modulo", "fiber_script_jit_bop_matches",
        "fiber_script_jit_bop_lt",     "fiber_script_jit_bop_lte",    "fiber_script_jit_bop_gt",
        "fiber_script_jit_bop_gte",    "fiber_script_jit_bop_eq",     "fiber_script_jit_bop_seq",
        "fiber_script_jit_bop_ne",     "fiber_script_jit_bop_sne",    "fiber_script_jit_bop_in",
};

constexpr std::array<const char *, kUnaryOpcodeCount> kUnaryHelperNames{
        "fiber_script_jit_unary_plus",
        "fiber_script_jit_unary_minus",
        "fiber_script_jit_unary_neg",
        "fiber_script_jit_unary_typeof",
};

std::atomic<std::uint64_t> g_module_id{1};

JitCompileError make_error(JitCompileStage stage, std::string message, std::uint32_t pc = ir::Compiled::kNoPc) {
    return JitCompileError{stage, std::move(message), pc};
}

std::string llvm_error_string(llvm::Error error) { return llvm::toString(std::move(error)); }

struct EngineState final {
    std::unique_ptr<llvm::orc::LLJIT> jit;
    std::shared_ptr<llvm_detail::StackMapCapturePlugin> stack_maps;
    std::mutex compile_mutex;
};

struct EngineHolder final {
    EngineHolder() {
#if !defined(__x86_64__) && !defined(__aarch64__)
        error = "script JIT supports only x86-64 and AArch64";
        return;
#endif
        if (llvm::InitializeNativeTarget() || llvm::InitializeNativeTargetAsmPrinter()) {
            error = "failed to initialize the native LLVM target";
            return;
        }
        auto created = llvm::orc::LLJITBuilder().create();
        if (!created) {
            error = llvm_error_string(created.takeError());
            return;
        }
        state = std::make_shared<EngineState>();
        state->jit = std::move(*created);
        state->stack_maps = std::make_shared<llvm_detail::StackMapCapturePlugin>();
        auto *object_layer = llvm::dyn_cast<llvm::orc::ObjectLinkingLayer>(&state->jit->getObjLinkingLayer());
        if (!object_layer) {
            error = "LLVM LLJIT is not using ObjectLinkingLayer";
            state.reset();
            return;
        }
        object_layer->addPlugin(state->stack_maps);

        llvm::orc::MangleAndInterner mangle(state->jit->getExecutionSession(), state->jit->getDataLayout());
        llvm::orc::SymbolMap symbols;
        auto add_symbol = [&](const char *name, auto *address) {
            symbols[mangle(name)] = llvm::orc::ExecutorSymbolDef(llvm::orc::ExecutorAddr::fromPtr(address),
                                                                 llvm::JITSymbolFlags::Exported);
        };
        add_symbol("fiber_script_jit_runtime_call", &run::fiber_script_jit_runtime_call);
        add_symbol("fiber_script_jit_logic", &run::fiber_script_jit_logic);
        add_symbol("fiber_script_jit_bop_plus", &run::fiber_script_jit_bop_plus);
        add_symbol("fiber_script_jit_bop_minus", &run::fiber_script_jit_bop_minus);
        add_symbol("fiber_script_jit_bop_multiply", &run::fiber_script_jit_bop_multiply);
        add_symbol("fiber_script_jit_bop_divide", &run::fiber_script_jit_bop_divide);
        add_symbol("fiber_script_jit_bop_modulo", &run::fiber_script_jit_bop_modulo);
        add_symbol("fiber_script_jit_bop_matches", &run::fiber_script_jit_bop_matches);
        add_symbol("fiber_script_jit_bop_lt", &run::fiber_script_jit_bop_lt);
        add_symbol("fiber_script_jit_bop_lte", &run::fiber_script_jit_bop_lte);
        add_symbol("fiber_script_jit_bop_gt", &run::fiber_script_jit_bop_gt);
        add_symbol("fiber_script_jit_bop_gte", &run::fiber_script_jit_bop_gte);
        add_symbol("fiber_script_jit_bop_eq", &run::fiber_script_jit_bop_eq);
        add_symbol("fiber_script_jit_bop_seq", &run::fiber_script_jit_bop_seq);
        add_symbol("fiber_script_jit_bop_ne", &run::fiber_script_jit_bop_ne);
        add_symbol("fiber_script_jit_bop_sne", &run::fiber_script_jit_bop_sne);
        add_symbol("fiber_script_jit_bop_in", &run::fiber_script_jit_bop_in);
        add_symbol("fiber_script_jit_unary_plus", &run::fiber_script_jit_unary_plus);
        add_symbol("fiber_script_jit_unary_minus", &run::fiber_script_jit_unary_minus);
        add_symbol("fiber_script_jit_unary_neg", &run::fiber_script_jit_unary_neg);
        add_symbol("fiber_script_jit_unary_typeof", &run::fiber_script_jit_unary_typeof);
        add_symbol("fiber_script_jit_iterate_next", &run::fiber_script_jit_iterate_next);
        if (llvm::Error define_error =
                    state->jit->getMainJITDylib().define(llvm::orc::absoluteSymbols(std::move(symbols)))) {
            error = llvm_error_string(std::move(define_error));
            state.reset();
        }
    }

    std::shared_ptr<EngineState> state;
    std::string error;
};

EngineHolder &engine_holder() {
    static EngineHolder holder;
    return holder;
}

struct ResourceOwner final {
    ~ResourceOwner() {
        if (tracker) {
            llvm::consumeError(tracker->remove());
        }
    }

    std::shared_ptr<EngineState> engine;
    llvm::orc::ResourceTrackerSP tracker;
};

struct LoweredEdge {
    BlockId predecessor = kInvalidBlock;
    BlockId successor = kInvalidBlock;
    EdgeKind kind = EdgeKind::Normal;
    llvm::BasicBlock *llvm_predecessor = nullptr;
    std::vector<std::pair<ValueId, llvm::Value *>> value_overrides;
};

struct LoweredModule {
    std::unique_ptr<llvm::LLVMContext> context;
    std::unique_ptr<llvm::Module> module;
    std::string entry_name;
    std::uint32_t async_value_count = 0;
    std::uint32_t statepoint_count = 0;
};

class ModuleLowerer final {
public:
    ModuleLowerer(const ir::Compiled &compiled, const Cfg &cfg, const Liveness &liveness, const AsyncSpill &spill,
                  const llvm::DataLayout &data_layout, llvm::StringRef target_triple, std::string entry_name) :
        compiled_(compiled), cfg_(cfg), liveness_(liveness), spill_(spill),
        context_(std::make_unique<llvm::LLVMContext>()),
        module_(std::make_unique<llvm::Module>("fiber.script.jit", *context_)), entry_name_(std::move(entry_name)) {
        module_->setDataLayout(data_layout);
        module_->setTargetTriple(llvm::Triple(target_triple));
        i1_ = Type::getInt1Ty(*context_);
        i8_ = Type::getInt8Ty(*context_);
        i32_ = Type::getInt32Ty(*context_);
        i64_ = Type::getInt64Ty(*context_);
        i128_ = Type::getInt128Ty(*context_);
        double_ = Type::getDoubleTy(*context_);
        ptr_ = llvm::PointerType::get(*context_, 0);
        managed_ptr_ = llvm::PointerType::get(*context_, 1);
        base_values_.assign(cfg_.values().size(), nullptr);
        exit_versions_.resize(cfg_.blocks().size(), std::vector<Value *>(cfg_.values().size(), nullptr));
        llvm_blocks_.assign(cfg_.blocks().size(), nullptr);
        persistent_slot_.assign(cfg_.values().size(), kInvalidSlot);
        for (std::uint32_t slot = 0; slot < spill_.persistent_values().size(); ++slot) {
            persistent_slot_[spill_.persistent_values()[slot]] = slot;
        }
    }

    std::expected<LoweredModule, JitCompileError> lower() {
        if (!declare_runtime() || !create_function() || !create_blocks() || !create_phis() || !emit_entry_dispatch() ||
            !emit_cfg_blocks() || !emit_resume_blocks() || !fill_cfg_phis()) {
            return std::unexpected(std::move(error_));
        }
        if (llvm::verifyModule(*module_, &llvm::errs())) {
            return std::unexpected(make_error(JitCompileStage::Verify, "LLVM verifier rejected generated IR"));
        }
        llvm::PassBuilder pass_builder;
        llvm::LoopAnalysisManager loop_analyses;
        llvm::FunctionAnalysisManager function_analyses;
        llvm::CGSCCAnalysisManager cgscc_analyses;
        llvm::ModuleAnalysisManager module_analyses;
        pass_builder.registerModuleAnalyses(module_analyses);
        pass_builder.registerCGSCCAnalyses(cgscc_analyses);
        pass_builder.registerFunctionAnalyses(function_analyses);
        pass_builder.registerLoopAnalyses(loop_analyses);
        pass_builder.crossRegisterProxies(loop_analyses, function_analyses, cgscc_analyses, module_analyses);
        llvm::ModulePassManager pipeline = pass_builder.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);
        pipeline.run(*module_, module_analyses);
        if (llvm::verifyModule(*module_, &llvm::errs())) {
            return std::unexpected(make_error(JitCompileStage::Optimize, "LLVM verifier rejected optimized JIT IR"));
        }
        statepoint_count_ = 0;
        for (const llvm::BasicBlock &block: *function_) {
            for (const llvm::Instruction &instruction: block) {
                const auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                if (call && call->getIntrinsicID() == llvm::Intrinsic::experimental_gc_statepoint) {
                    ++statepoint_count_;
                }
            }
        }
        LoweredModule result;
        result.context = std::move(context_);
        result.module = std::move(module_);
        result.entry_name = std::move(entry_name_);
        result.async_value_count = static_cast<std::uint32_t>(spill_.persistent_values().size());
        result.statepoint_count = statepoint_count_;
        return result;
    }

private:
    const ir::Compiled &compiled_;
    const Cfg &cfg_;
    const Liveness &liveness_;
    const AsyncSpill &spill_;
    std::unique_ptr<llvm::LLVMContext> context_;
    std::unique_ptr<llvm::Module> module_;
    std::string entry_name_;
    JitCompileError error_;

    Type *i1_ = nullptr;
    Type *i8_ = nullptr;
    Type *i32_ = nullptr;
    Type *i64_ = nullptr;
    Type *i128_ = nullptr;
    Type *double_ = nullptr;
    llvm::PointerType *ptr_ = nullptr;
    llvm::PointerType *managed_ptr_ = nullptr;
    Function *function_ = nullptr;
    Value *frame_ = nullptr;
    llvm::AllocaInst *argument_scratch_ = nullptr;
    llvm::AllocaInst *out_scratch_ = nullptr;
    llvm::FunctionCallee runtime_call_;
    llvm::FunctionCallee logic_call_;
    std::array<llvm::FunctionCallee, kBinaryOpcodeCount> binary_calls_;
    std::array<llvm::FunctionCallee, kUnaryOpcodeCount> unary_calls_;
    llvm::FunctionCallee iterate_next_call_;
    std::vector<llvm::BasicBlock *> llvm_blocks_;
    std::vector<llvm::BasicBlock *> resume_blocks_;
    std::vector<Value *> base_values_;
    std::vector<std::vector<Value *>> exit_versions_;
    std::vector<std::uint32_t> persistent_slot_;
    std::vector<LoweredEdge> lowered_edges_;
    std::uint32_t max_argument_count_ = 1;
    std::uint32_t statepoint_count_ = 0;

    bool fail(JitCompileStage stage, const char *message, std::uint32_t pc = ir::Compiled::kNoPc) {
        error_ = make_error(stage, message, pc);
        return false;
    }

    llvm::ConstantInt *constant_i32(std::uint32_t value) const {
        return llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(i32_), value);
    }

    llvm::ConstantInt *constant_i64(std::uint64_t value) const {
        return llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(i64_), value);
    }

    llvm::ConstantInt *boxed_constant(const JsValue &value) const {
        std::array<std::uint64_t, 2> words{};
        std::memcpy(words.data(), &value, sizeof(value));
        return llvm::ConstantInt::get(*context_, llvm::APInt(128, words));
    }

    llvm::ConstantInt *boxed_tag_bits(JsTag tag) const {
        llvm::APInt bits(128, static_cast<std::uint8_t>(tag));
        bits <<= 112u;
        return llvm::ConstantInt::get(*context_, bits);
    }

    Value *boxed_tag(IRBuilder<> &builder, Value *boxed, const llvm::Twine &name = "") const {
        Value *bits = builder.CreateLShr(boxed, llvm::ConstantInt::get(i128_, 112));
        return builder.CreateTrunc(bits, i8_, name);
    }

    Value *boxed_payload(IRBuilder<> &builder, Value *boxed, const llvm::Twine &name = "") const {
        return builder.CreateTrunc(boxed, i64_, name);
    }

    Value *box_payload(IRBuilder<> &builder, Value *payload, JsTag tag, const llvm::Twine &name = "") const {
        Value *boxed = builder.CreateZExt(payload, i128_);
        return builder.CreateOr(boxed, boxed_tag_bits(tag), name);
    }

    Value *box_boolean(IRBuilder<> &builder, Value *value, const llvm::Twine &name = "") const {
        return box_payload(builder, builder.CreateZExt(value, i64_), JsTag::Boolean, name);
    }

    Value *box_double(IRBuilder<> &builder, Value *value, const llvm::Twine &name = "") const {
        return box_payload(builder, builder.CreateBitCast(value, i64_), JsTag::Double, name);
    }

    Value *frame_field(IRBuilder<> &builder, std::size_t offset) const {
        return builder.CreateConstGEP1_64(i8_, frame_, offset);
    }

    Value *load_frame_i32(IRBuilder<> &builder, std::size_t offset, const llvm::Twine &name = "") const {
        return builder.CreateAlignedLoad(i32_, frame_field(builder, offset), llvm::Align(4), name);
    }

    void store_frame_i32(IRBuilder<> &builder, std::size_t offset, std::uint32_t value) const {
        builder.CreateAlignedStore(constant_i32(value), frame_field(builder, offset), llvm::Align(4));
    }

    Value *load_frame_boxed(IRBuilder<> &builder, std::size_t offset, const llvm::Twine &name = "") const {
        return builder.CreateAlignedLoad(i128_, frame_field(builder, offset), llvm::Align(16), name);
    }

    void store_frame_boxed(IRBuilder<> &builder, std::size_t offset, Value *value) const {
        builder.CreateAlignedStore(value, frame_field(builder, offset), llvm::Align(16));
    }

    void store_state(IRBuilder<> &builder, run::JitRunState state) const {
        store_frame_i32(builder, offsetof(run::JitFrameHeader, state), static_cast<std::uint32_t>(state));
    }

    void emit_abort(IRBuilder<> &builder, ScriptAbortReason reason, std::int64_t position = -1) const {
        Value *reason_ptr = frame_field(builder, offsetof(run::JitFrameHeader, abort) + offsetof(ScriptAbort, reason));
        builder.CreateAlignedStore(
                llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(i8_), static_cast<std::uint8_t>(reason)),
                reason_ptr, llvm::Align(1));
        Value *position_ptr =
                frame_field(builder, offsetof(run::JitFrameHeader, abort) + offsetof(ScriptAbort, position));
        builder.CreateAlignedStore(llvm::ConstantInt::getSigned(llvm::cast<llvm::IntegerType>(i64_), position),
                                   position_ptr, llvm::Align(8));
    }

    void fill_abort_position(IRBuilder<> &builder, std::uint32_t pc) const {
        std::uint32_t source_position = compiled_.find_position(pc);
        if (source_position == ir::Compiled::kNoPc) {
            return;
        }
        Value *position_ptr =
                frame_field(builder, offsetof(run::JitFrameHeader, abort) + offsetof(ScriptAbort, position));
        Value *position = builder.CreateAlignedLoad(i64_, position_ptr, llvm::Align(8), "abort.position");
        Value *missing =
                builder.CreateICmpSLT(position, llvm::ConstantInt::getSigned(llvm::cast<llvm::IntegerType>(i64_), 0));
        Value *resolved = builder.CreateSelect(missing, constant_i64(source_position), position);
        builder.CreateAlignedStore(resolved, position_ptr, llvm::Align(8));
    }

    void emit_return(IRBuilder<> &builder, run::JitStatus status) const {
        builder.CreateRet(constant_i32(static_cast<std::uint32_t>(status)));
    }

    bool declare_runtime() {
        auto *runtime_type = llvm::FunctionType::get(i32_, {ptr_, i32_, i32_, ptr_, i32_, ptr_}, false);
        runtime_call_ = module_->getOrInsertFunction("fiber_script_jit_runtime_call", runtime_type);
        llvm::cast<Function>(runtime_call_.getCallee())->addFnAttr(llvm::Attribute::NoUnwind);
        auto *logic_type = llvm::FunctionType::get(i32_, {ptr_}, false);
        logic_call_ = module_->getOrInsertFunction("fiber_script_jit_logic", logic_type);
        llvm::cast<Function>(logic_call_.getCallee())->addFnAttr(llvm::Attribute::NoUnwind);
        auto *exact_type = llvm::FunctionType::get(i32_, {ptr_, ptr_, ptr_}, false);
        for (std::size_t i = 0; i < binary_calls_.size(); ++i) {
            binary_calls_[i] = module_->getOrInsertFunction(kBinaryHelperNames[i], exact_type);
            llvm::cast<Function>(binary_calls_[i].getCallee())->addFnAttr(llvm::Attribute::NoUnwind);
        }
        for (std::size_t i = 0; i < unary_calls_.size(); ++i) {
            unary_calls_[i] = module_->getOrInsertFunction(kUnaryHelperNames[i], exact_type);
            llvm::cast<Function>(unary_calls_[i].getCallee())->addFnAttr(llvm::Attribute::NoUnwind);
        }
        iterate_next_call_ = module_->getOrInsertFunction("fiber_script_jit_iterate_next", exact_type);
        llvm::cast<Function>(iterate_next_call_.getCallee())->addFnAttr(llvm::Attribute::NoUnwind);
        return true;
    }

    bool create_function() {
        for (const BasicBlock &block: cfg_.blocks()) {
            for (const Instruction &instruction: block.instructions) {
                max_argument_count_ =
                        std::max(max_argument_count_, static_cast<std::uint32_t>(instruction.operands.size()));
            }
        }
        auto *function_type = llvm::FunctionType::get(i32_, {ptr_}, false);
        function_ = Function::Create(function_type, llvm::GlobalValue::ExternalLinkage, entry_name_, *module_);
        function_->setGC("statepoint-example");
        function_->addFnAttr(llvm::Attribute::NoUnwind);
        function_->addFnAttr("frame-pointer", "none");
        frame_ = function_->getArg(0);
        frame_->setName("frame");
        return true;
    }

    bool create_blocks() {
        for (const BasicBlock &block: cfg_.blocks()) {
            if (block.reachable) {
                llvm_blocks_[block.id] = llvm::BasicBlock::Create(*context_, "b" + std::to_string(block.id), function_);
            }
        }
        resume_blocks_.reserve(spill_.sites().size());
        for (const AsyncSiteSpill &site: spill_.sites()) {
            resume_blocks_.push_back(
                    llvm::BasicBlock::Create(*context_, "resume." + std::to_string(site.resume_id), function_));
        }
        return true;
    }

    bool create_phis() {
        base_values_[cfg_.undefined_value()] = boxed_constant(JsValue::make_undefined());
        for (const BasicBlock &block: cfg_.blocks()) {
            if (!block.reachable) {
                continue;
            }
            IRBuilder<> builder(llvm_blocks_[block.id]);
            for (const Phi &phi: block.phis) {
                auto *node = builder.CreatePHI(i128_, static_cast<unsigned>(phi.incoming.size() + 1u),
                                               "v" + std::to_string(phi.result));
                base_values_[phi.result] = node;
            }
        }
        return true;
    }

    bool emit_entry_dispatch() {
        llvm::BasicBlock *entry =
                llvm::BasicBlock::Create(*context_, "entry", function_, llvm_blocks_[cfg_.entry_block()]);
        llvm::BasicBlock *valid_abi =
                llvm::BasicBlock::Create(*context_, "entry.valid", function_, llvm_blocks_[cfg_.entry_block()]);
        llvm::BasicBlock *invalid =
                llvm::BasicBlock::Create(*context_, "entry.invalid", function_, llvm_blocks_[cfg_.entry_block()]);
        llvm::BasicBlock *initial =
                llvm::BasicBlock::Create(*context_, "entry.initial", function_, llvm_blocks_[cfg_.entry_block()]);
        IRBuilder<> builder(entry);
        argument_scratch_ = builder.CreateAlloca(i128_, constant_i32(max_argument_count_), "arguments");
        argument_scratch_->setAlignment(llvm::Align(16));
        out_scratch_ = builder.CreateAlloca(i128_, nullptr, "out");
        out_scratch_->setAlignment(llvm::Align(16));
        Value *abi = load_frame_i32(builder, offsetof(run::JitFrameHeader, abi_version), "abi");
        Value *abi_ok = builder.CreateICmpEQ(abi, constant_i32(run::kJitAbiVersion));
        builder.CreateCondBr(abi_ok, valid_abi, invalid);

        builder.SetInsertPoint(invalid);
        emit_abort(builder, ScriptAbortReason::InvalidState);
        emit_return(builder, run::JitStatus::Abort);

        builder.SetInsertPoint(valid_abi);
        Value *resume_id = load_frame_i32(builder, offsetof(run::JitFrameHeader, resume_id), "resume.id");
        auto *dispatch = builder.CreateSwitch(resume_id, invalid, static_cast<unsigned>(spill_.sites().size() + 1u));
        dispatch->addCase(constant_i32(0), initial);
        for (std::size_t i = 0; i < spill_.sites().size(); ++i) {
            dispatch->addCase(constant_i32(spill_.sites()[i].resume_id), resume_blocks_[i]);
        }

        builder.SetInsertPoint(initial);
        store_state(builder, run::JitRunState::Running);
        builder.CreateBr(llvm_blocks_[cfg_.entry_block()]);
        return true;
    }

    Value *version_for(const std::vector<Value *> &versions, ValueId id, std::uint32_t pc) {
        if (id >= versions.size()) {
            fail(JitCompileStage::LlvmIr, "SSA operand is out of range", pc);
            return nullptr;
        }
        Value *value = versions[id] ? versions[id] : base_values_[id];
        if (!value) {
            fail(JitCompileStage::LlvmIr, "SSA operand has no dominating definition", pc);
        }
        return value;
    }

    Value *argument_pointer(IRBuilder<> &builder, std::uint32_t index) const {
        return builder.CreateConstGEP1_32(i128_, argument_scratch_, index);
    }

    void store_arguments(IRBuilder<> &builder, const Instruction &instruction, const std::vector<Value *> &versions) {
        for (std::uint32_t i = 0; i < instruction.operands.size(); ++i) {
            Value *operand = version_for(versions, instruction.operands[i], instruction.pc);
            if (!operand) {
                return;
            }
            builder.CreateAlignedStore(operand, argument_pointer(builder, i), llvm::Align(16));
        }
    }

    Value *load_persistent(IRBuilder<> &builder, std::uint32_t slot, const llvm::Twine &name = "") const {
        Value *base = builder.CreateAlignedLoad(ptr_, frame_field(builder, offsetof(run::JitFrameHeader, async_values)),
                                                llvm::Align(8), "async.values");
        Value *address = builder.CreateConstGEP1_32(i128_, base, slot);
        return builder.CreateAlignedLoad(i128_, address, llvm::Align(16), name);
    }

    void store_persistent(IRBuilder<> &builder, std::uint32_t slot, Value *value) const {
        Value *base = builder.CreateAlignedLoad(ptr_, frame_field(builder, offsetof(run::JitFrameHeader, async_values)),
                                                llvm::Align(8), "async.values");
        Value *address = builder.CreateConstGEP1_32(i128_, base, slot);
        builder.CreateAlignedStore(value, address, llvm::Align(16));
    }

    struct ManagedRoot {
        ValueId id = kInvalidValue;
        Value *boxed = nullptr;
        Value *is_heap = nullptr;
        Value *pointer = nullptr;
    };

    std::vector<ManagedRoot> create_roots(IRBuilder<> &builder, const BasicBlock &block,
                                          std::uint32_t instruction_index, const std::vector<Value *> &versions) {
        ValueBitSet roots = liveness_.block(block.id).live_before[instruction_index];
        std::vector<ManagedRoot> result;
        for (ValueId id: roots.values()) {
            if ((cfg_.values()[id].type_mask & TypeHeapRef) == 0) {
                continue;
            }
            Value *boxed = version_for(versions, id, block.instructions[instruction_index].pc);
            if (!boxed) {
                return {};
            }
            Value *tag_bits = builder.CreateLShr(boxed, llvm::ConstantInt::get(i128_, 112));
            Value *tag = builder.CreateTrunc(tag_bits, i8_);
            Value *is_heap =
                    builder.CreateICmpEQ(tag, llvm::ConstantInt::get(i8_, static_cast<std::uint8_t>(JsTag::HeapRef)));
            Value *payload = builder.CreateTrunc(boxed, i64_);
            Value *pointer = builder.CreateIntToPtr(payload, managed_ptr_);
            pointer = builder.CreateSelect(is_heap, pointer, llvm::ConstantPointerNull::get(managed_ptr_));
            result.push_back(ManagedRoot{id, boxed, is_heap, pointer});
            if (result.size() > kMaxStatepointRoots) {
                fail(JitCompileStage::LlvmIr, "statepoint root count exceeds JIT limit",
                     block.instructions[instruction_index].pc);
                return {};
            }
        }
        return result;
    }

    Value *relocated_boxed(IRBuilder<> &builder, const ManagedRoot &root, Value *relocated) const {
        llvm::APInt payload_mask = llvm::APInt::getLowBitsSet(128, 64);
        Value *metadata = builder.CreateAnd(root.boxed, llvm::ConstantInt::get(*context_, ~payload_mask));
        Value *payload = builder.CreateZExt(builder.CreatePtrToInt(relocated, i64_), i128_);
        Value *updated = builder.CreateOr(metadata, payload);
        return builder.CreateSelect(root.is_heap, updated, root.boxed);
    }

    Value *emit_statepoint_call(IRBuilder<> &builder, const BasicBlock &block, std::uint32_t instruction_index,
                                const Instruction &instruction, llvm::FunctionCallee callee,
                                llvm::ArrayRef<Value *> call_arguments, std::vector<Value *> &versions) {
        std::vector<ManagedRoot> roots = create_roots(builder, block, instruction_index, versions);
        if (!error_.message.empty()) {
            return nullptr;
        }
        std::vector<Value *> gc_arguments;
        gc_arguments.reserve(roots.size());
        for (const ManagedRoot &root: roots) {
            gc_arguments.push_back(root.pointer);
        }
        auto *statepoint = builder.CreateGCStatepointCall(++statepoint_count_, 0, callee, call_arguments, std::nullopt,
                                                          gc_arguments, "statepoint");
        Function *fake_use = llvm::Intrinsic::getOrInsertDeclaration(module_.get(), llvm::Intrinsic::fake_use);
        for (std::uint32_t i = 0; i < roots.size(); ++i) {
            Value *relocated = builder.CreateGCRelocate(statepoint, i, i, managed_ptr_, "relocated");
            builder.CreateCall(fake_use, {relocated});
            versions[roots[i].id] = relocated_boxed(builder, roots[i], relocated);
        }
        return builder.CreateGCResult(statepoint, i32_, "runtime.status");
    }

    Value *emit_generic_statepoint_call(IRBuilder<> &builder, const BasicBlock &block, std::uint32_t instruction_index,
                                        const Instruction &instruction, std::vector<Value *> &versions) {
        std::array<Value *, 6> call_arguments{
                frame_,
                constant_i32(instruction.opcode),
                constant_i32(instruction.raw),
                argument_scratch_,
                constant_i32(static_cast<std::uint32_t>(instruction.operands.size())),
                out_scratch_,
        };
        return emit_statepoint_call(builder, block, instruction_index, instruction, runtime_call_, call_arguments,
                                    versions);
    }

    void record_edge(BlockId predecessor, BlockId successor, EdgeKind kind, llvm::BasicBlock *llvm_predecessor,
                     std::vector<std::pair<ValueId, Value *>> value_overrides = {}) {
        lowered_edges_.push_back(
                LoweredEdge{predecessor, successor, kind, llvm_predecessor, std::move(value_overrides)});
    }

    llvm::FunctionCallee binary_call(std::uint8_t opcode) const {
        return binary_calls_[static_cast<std::size_t>(opcode - ir::Code::BOP_PLUS)];
    }

    llvm::FunctionCallee unary_call(std::uint8_t opcode) const {
        return unary_calls_[static_cast<std::size_t>(opcode - ir::Code::UNARY_PLUS)];
    }

    bool emit_exact_status_paths(IRBuilder<> &builder, const BasicBlock &block, const Instruction &instruction,
                                 Value *status, Value *out, llvm::BasicBlock *success_target, bool record_normal_edge,
                                 std::vector<std::pair<ValueId, Value *>> exception_overrides = {},
                                 llvm::BasicBlock **success_block_out = nullptr) {
        llvm::BasicBlock *success = llvm::BasicBlock::Create(*context_, "exact.success", function_);
        llvm::BasicBlock *exception = llvm::BasicBlock::Create(*context_, "exact.exception", function_);
        llvm::BasicBlock *abort = llvm::BasicBlock::Create(*context_, "exact.abort", function_);
        llvm::BasicBlock *invalid = llvm::BasicBlock::Create(*context_, "exact.invalid", function_);
        auto *status_switch = builder.CreateSwitch(status, invalid, 3);
        status_switch->addCase(constant_i32(static_cast<std::uint32_t>(run::JitStatus::Success)), success);
        status_switch->addCase(constant_i32(static_cast<std::uint32_t>(run::JitStatus::Exception)), exception);
        status_switch->addCase(constant_i32(static_cast<std::uint32_t>(run::JitStatus::Abort)), abort);

        builder.SetInsertPoint(success);
        builder.CreateBr(success_target);
        if (record_normal_edge) {
            record_edge(block.id, instruction.normal_target, EdgeKind::Normal, success);
        }
        if (success_block_out) {
            *success_block_out = success;
        }

        builder.SetInsertPoint(exception);
        if (instruction.exception_target != kInvalidBlock) {
            builder.CreateBr(llvm_blocks_[instruction.exception_target]);
            record_edge(block.id, instruction.exception_target, EdgeKind::Exception, exception,
                        std::move(exception_overrides));
        } else {
            store_frame_boxed(builder, offsetof(run::JitFrameHeader, pending_exception), out);
            emit_return(builder, run::JitStatus::Exception);
        }

        builder.SetInsertPoint(abort);
        emit_return(builder, run::JitStatus::Abort);

        builder.SetInsertPoint(invalid);
        emit_abort(builder, ScriptAbortReason::Internal);
        emit_return(builder, run::JitStatus::Abort);
        return true;
    }

    bool has_integer_binary_fast_path(std::uint8_t opcode) const noexcept {
        return (opcode >= ir::Code::BOP_PLUS && opcode <= ir::Code::BOP_MOD) ||
               (opcode >= ir::Code::BOP_LT && opcode <= ir::Code::BOP_SNE);
    }

    Value *integer_binary_guard(IRBuilder<> &builder, std::uint8_t opcode, Value *lhs, Value *rhs) const {
        Value *lhs_is_integer = builder.CreateICmpEQ(
                boxed_tag(builder, lhs), llvm::ConstantInt::get(i8_, static_cast<std::uint8_t>(JsTag::Int64)));
        Value *rhs_is_integer = builder.CreateICmpEQ(
                boxed_tag(builder, rhs), llvm::ConstantInt::get(i8_, static_cast<std::uint8_t>(JsTag::Int64)));
        Value *guard = builder.CreateAnd(lhs_is_integer, rhs_is_integer, "both.integer");
        if (opcode == ir::Code::BOP_DIVIDE || opcode == ir::Code::BOP_MOD) {
            Value *rhs_payload = boxed_payload(builder, rhs);
            guard = builder.CreateAnd(guard, builder.CreateICmpNE(rhs_payload, constant_i64(0)), "nonzero.rhs");
            if (opcode == ir::Code::BOP_MOD) {
                Value *lhs_is_min =
                        builder.CreateICmpEQ(boxed_payload(builder, lhs), constant_i64(std::uint64_t{1} << 63u));
                Value *rhs_is_minus_one = builder.CreateICmpEQ(rhs_payload, constant_i64(UINT64_MAX));
                guard = builder.CreateAnd(guard, builder.CreateNot(builder.CreateAnd(lhs_is_min, rhs_is_minus_one)),
                                          "safe.srem");
            }
        }
        return guard;
    }

    Value *emit_integer_binary_result(IRBuilder<> &builder, std::uint8_t opcode, Value *lhs_boxed, Value *rhs_boxed) {
        Value *lhs = boxed_payload(builder, lhs_boxed, "lhs.integer");
        Value *rhs = boxed_payload(builder, rhs_boxed, "rhs.integer");
        if (opcode == ir::Code::BOP_PLUS || opcode == ir::Code::BOP_MINUS || opcode == ir::Code::BOP_MULTIPLY) {
            llvm::Intrinsic::ID intrinsic_id = llvm::Intrinsic::sadd_with_overflow;
            if (opcode == ir::Code::BOP_MINUS) {
                intrinsic_id = llvm::Intrinsic::ssub_with_overflow;
            } else if (opcode == ir::Code::BOP_MULTIPLY) {
                intrinsic_id = llvm::Intrinsic::smul_with_overflow;
            }
            Function *intrinsic = llvm::Intrinsic::getOrInsertDeclaration(module_.get(), intrinsic_id, {i64_});
            Value *pair = builder.CreateCall(intrinsic, {lhs, rhs}, "integer.operation");
            Value *integer_result = builder.CreateExtractValue(pair, 0);
            Value *overflow = builder.CreateExtractValue(pair, 1);
            Value *boxed_integer = box_payload(builder, integer_result, JsTag::Int64);
            Value *lhs_float = builder.CreateSIToFP(lhs, double_);
            Value *rhs_float = builder.CreateSIToFP(rhs, double_);
            Value *float_result = nullptr;
            if (opcode == ir::Code::BOP_PLUS) {
                float_result = builder.CreateFAdd(lhs_float, rhs_float);
            } else if (opcode == ir::Code::BOP_MINUS) {
                float_result = builder.CreateFSub(lhs_float, rhs_float);
            } else {
                float_result = builder.CreateFMul(lhs_float, rhs_float);
            }
            return builder.CreateSelect(overflow, box_double(builder, float_result), boxed_integer, "boxed.number");
        }
        if (opcode == ir::Code::BOP_DIVIDE) {
            Value *result = builder.CreateFDiv(builder.CreateSIToFP(lhs, double_), builder.CreateSIToFP(rhs, double_));
            return box_double(builder, result, "boxed.divide");
        }
        if (opcode == ir::Code::BOP_MOD) {
            return box_payload(builder, builder.CreateSRem(lhs, rhs), JsTag::Int64, "boxed.modulo");
        }

        Value *lhs_float = builder.CreateSIToFP(lhs, double_);
        Value *rhs_float = builder.CreateSIToFP(rhs, double_);
        Value *comparison = nullptr;
        switch (opcode) {
            case ir::Code::BOP_LT:
                comparison = builder.CreateFCmpOLT(lhs_float, rhs_float);
                break;
            case ir::Code::BOP_LTE:
                comparison = builder.CreateFCmpOLE(lhs_float, rhs_float);
                break;
            case ir::Code::BOP_GT:
                comparison = builder.CreateFCmpOGT(lhs_float, rhs_float);
                break;
            case ir::Code::BOP_GTE:
                comparison = builder.CreateFCmpOGE(lhs_float, rhs_float);
                break;
            case ir::Code::BOP_EQ:
            case ir::Code::BOP_SEQ:
                comparison = builder.CreateFCmpOEQ(lhs_float, rhs_float);
                break;
            case ir::Code::BOP_NE:
            case ir::Code::BOP_SNE:
                comparison = builder.CreateFCmpUNE(lhs_float, rhs_float);
                break;
            default:
                return nullptr;
        }
        return box_boolean(builder, comparison, "boxed.comparison");
    }

    bool emit_binary_instruction(IRBuilder<> &builder, const BasicBlock &block, std::uint32_t instruction_index,
                                 const Instruction &instruction, std::vector<Value *> &versions) {
        Value *lhs = version_for(versions, instruction.operands[0], instruction.pc);
        Value *rhs = version_for(versions, instruction.operands[1], instruction.pc);
        if (!lhs || !rhs) {
            return false;
        }
        if (!has_integer_binary_fast_path(instruction.opcode)) {
            store_arguments(builder, instruction, versions);
            store_frame_i32(builder, offsetof(run::JitFrameHeader, active_pc), instruction.pc);
            Value *status = builder.CreateCall(binary_call(instruction.opcode),
                                               {frame_, argument_scratch_, out_scratch_}, "exact.status");
            Value *out = builder.CreateAlignedLoad(i128_, out_scratch_, llvm::Align(16), "exact.out");
            if (instruction.result != kInvalidValue) {
                versions[instruction.result] = out;
                base_values_[instruction.result] = out;
            }
            if (instruction.exception != kInvalidValue) {
                versions[instruction.exception] = out;
                base_values_[instruction.exception] = out;
            }
            if (instruction.normal_target == kInvalidBlock) {
                return fail(JitCompileStage::LlvmIr, "binary instruction has no continuation", instruction.pc);
            }
            return emit_exact_status_paths(builder, block, instruction, status, out,
                                           llvm_blocks_[instruction.normal_target], true);
        }

        llvm::BasicBlock *fast = llvm::BasicBlock::Create(*context_, "binary.integer", function_);
        llvm::BasicBlock *slow = llvm::BasicBlock::Create(*context_, "binary.slow", function_);
        llvm::BasicBlock *merge = llvm::BasicBlock::Create(*context_, "binary.merge", function_);
        Value *guard = integer_binary_guard(builder, instruction.opcode, lhs, rhs);
        builder.CreateCondBr(guard, fast, slow);

        builder.SetInsertPoint(fast);
        Value *fast_result = emit_integer_binary_result(builder, instruction.opcode, lhs, rhs);
        if (!fast_result) {
            return fail(JitCompileStage::LlvmIr, "invalid native binary opcode", instruction.pc);
        }
        builder.CreateBr(merge);
        llvm::BasicBlock *fast_predecessor = builder.GetInsertBlock();

        builder.SetInsertPoint(slow);
        store_arguments(builder, instruction, versions);
        store_frame_i32(builder, offsetof(run::JitFrameHeader, active_pc), instruction.pc);
        std::vector<Value *> fast_versions = versions;
        Value *status = nullptr;
        if (has_effect(instruction.effects, Effect::MayGC)) {
            std::array<Value *, 3> call_arguments{frame_, argument_scratch_, out_scratch_};
            status = emit_statepoint_call(builder, block, instruction_index, instruction,
                                          binary_call(instruction.opcode), call_arguments, versions);
        } else {
            status = builder.CreateCall(binary_call(instruction.opcode), {frame_, argument_scratch_, out_scratch_},
                                        "exact.status");
        }
        if (!status) {
            return false;
        }
        std::vector<std::pair<ValueId, Value *>> relocated_values;
        for (ValueId id = 0; id < versions.size(); ++id) {
            if (versions[id] != fast_versions[id] && versions[id]) {
                relocated_values.emplace_back(id, versions[id]);
            }
        }
        Value *slow_result = builder.CreateAlignedLoad(i128_, out_scratch_, llvm::Align(16), "exact.out");
        if (instruction.exception != kInvalidValue) {
            versions[instruction.exception] = slow_result;
            base_values_[instruction.exception] = slow_result;
        }
        llvm::BasicBlock *slow_success = nullptr;
        if (!emit_exact_status_paths(builder, block, instruction, status, slow_result, merge, false, relocated_values,
                                     &slow_success)) {
            return false;
        }

        builder.SetInsertPoint(merge);
        for (const auto &[id, slow_value]: relocated_values) {
            Value *fast_value = fast_versions[id] ? fast_versions[id] : base_values_[id];
            if (!fast_value) {
                return fail(JitCompileStage::LlvmIr, "relocated value has no fast-path version", instruction.pc);
            }
            auto *phi = builder.CreatePHI(i128_, 2, "relocated.merge");
            phi->addIncoming(fast_value, fast_predecessor);
            phi->addIncoming(slow_value, slow_success);
            versions[id] = phi;
        }
        auto *result = builder.CreatePHI(i128_, 2, "binary.result");
        result->addIncoming(fast_result, fast_predecessor);
        result->addIncoming(slow_result, slow_success);
        versions[instruction.result] = result;
        base_values_[instruction.result] = result;
        if (instruction.normal_target == kInvalidBlock) {
            return fail(JitCompileStage::LlvmIr, "binary instruction has no continuation", instruction.pc);
        }
        builder.CreateBr(llvm_blocks_[instruction.normal_target]);
        record_edge(block.id, instruction.normal_target, EdgeKind::Normal, merge);
        return true;
    }

    bool has_integer_unary_fast_path(std::uint8_t opcode) const noexcept {
        return opcode == ir::Code::UNARY_PLUS || opcode == ir::Code::UNARY_MINUS || opcode == ir::Code::UNARY_NEG;
    }

    Value *emit_integer_unary_result(IRBuilder<> &builder, std::uint8_t opcode, Value *boxed) {
        Value *payload = boxed_payload(builder, boxed, "unary.integer");
        if (opcode == ir::Code::UNARY_PLUS) {
            return boxed;
        }
        if (opcode == ir::Code::UNARY_NEG) {
            return box_boolean(builder, builder.CreateICmpEQ(payload, constant_i64(0)), "boxed.not");
        }
        Function *intrinsic =
                llvm::Intrinsic::getOrInsertDeclaration(module_.get(), llvm::Intrinsic::ssub_with_overflow, {i64_});
        Value *pair = builder.CreateCall(intrinsic, {constant_i64(0), payload}, "integer.negate");
        Value *integer_result = builder.CreateExtractValue(pair, 0);
        Value *overflow = builder.CreateExtractValue(pair, 1);
        Value *float_result = builder.CreateFNeg(builder.CreateSIToFP(payload, double_));
        return builder.CreateSelect(overflow, box_double(builder, float_result),
                                    box_payload(builder, integer_result, JsTag::Int64), "boxed.negate");
    }

    bool emit_unary_instruction(IRBuilder<> &builder, const BasicBlock &block, const Instruction &instruction,
                                std::vector<Value *> &versions) {
        Value *operand = version_for(versions, instruction.operands[0], instruction.pc);
        if (!operand) {
            return false;
        }
        if (!has_integer_unary_fast_path(instruction.opcode)) {
            store_arguments(builder, instruction, versions);
            store_frame_i32(builder, offsetof(run::JitFrameHeader, active_pc), instruction.pc);
            Value *status = builder.CreateCall(unary_call(instruction.opcode),
                                               {frame_, argument_scratch_, out_scratch_}, "exact.status");
            Value *out = builder.CreateAlignedLoad(i128_, out_scratch_, llvm::Align(16), "exact.out");
            versions[instruction.result] = out;
            base_values_[instruction.result] = out;
            if (instruction.exception != kInvalidValue) {
                versions[instruction.exception] = out;
                base_values_[instruction.exception] = out;
            }
            return emit_exact_status_paths(builder, block, instruction, status, out,
                                           llvm_blocks_[instruction.normal_target], true);
        }

        llvm::BasicBlock *fast = llvm::BasicBlock::Create(*context_, "unary.integer", function_);
        llvm::BasicBlock *slow = llvm::BasicBlock::Create(*context_, "unary.slow", function_);
        llvm::BasicBlock *merge = llvm::BasicBlock::Create(*context_, "unary.merge", function_);
        Value *is_integer = builder.CreateICmpEQ(boxed_tag(builder, operand),
                                                 llvm::ConstantInt::get(i8_, static_cast<std::uint8_t>(JsTag::Int64)));
        builder.CreateCondBr(is_integer, fast, slow);

        builder.SetInsertPoint(fast);
        Value *fast_result = emit_integer_unary_result(builder, instruction.opcode, operand);
        builder.CreateBr(merge);
        llvm::BasicBlock *fast_predecessor = builder.GetInsertBlock();

        builder.SetInsertPoint(slow);
        store_arguments(builder, instruction, versions);
        store_frame_i32(builder, offsetof(run::JitFrameHeader, active_pc), instruction.pc);
        Value *status = builder.CreateCall(unary_call(instruction.opcode), {frame_, argument_scratch_, out_scratch_},
                                           "exact.status");
        Value *slow_result = builder.CreateAlignedLoad(i128_, out_scratch_, llvm::Align(16), "exact.out");
        if (instruction.exception != kInvalidValue) {
            versions[instruction.exception] = slow_result;
            base_values_[instruction.exception] = slow_result;
        }
        llvm::BasicBlock *slow_success = nullptr;
        if (!emit_exact_status_paths(builder, block, instruction, status, slow_result, merge, false, {},
                                     &slow_success)) {
            return false;
        }

        builder.SetInsertPoint(merge);
        auto *result = builder.CreatePHI(i128_, 2, "unary.result");
        result->addIncoming(fast_result, fast_predecessor);
        result->addIncoming(slow_result, slow_success);
        versions[instruction.result] = result;
        base_values_[instruction.result] = result;
        builder.CreateBr(llvm_blocks_[instruction.normal_target]);
        record_edge(block.id, instruction.normal_target, EdgeKind::Normal, merge);
        return true;
    }

    bool emit_iterate_next_instruction(IRBuilder<> &builder, const BasicBlock &block, const Instruction &instruction,
                                       std::vector<Value *> &versions) {
        Value *iterator_boxed = version_for(versions, instruction.operands[0], instruction.pc);
        if (!iterator_boxed) {
            return false;
        }
        Value *tag = boxed_tag(builder, iterator_boxed, "iterator.tag");
        Value *subtag_bits = builder.CreateLShr(iterator_boxed, llvm::ConstantInt::get(i128_, 120));
        Value *subtag = builder.CreateTrunc(subtag_bits, i8_, "iterator.subtag");
        Value *payload = boxed_payload(builder, iterator_boxed, "iterator.payload");
        Value *is_heap =
                builder.CreateICmpEQ(tag, llvm::ConstantInt::get(i8_, static_cast<std::uint8_t>(JsTag::HeapRef)));
        Value *is_iterator = builder.CreateICmpEQ(
                subtag, llvm::ConstantInt::get(i8_, static_cast<std::uint8_t>(GcHeapKind::Iterator)));
        Value *has_pointer = builder.CreateICmpNE(payload, constant_i64(0));
        Value *valid = builder.CreateAnd(builder.CreateAnd(is_heap, is_iterator), has_pointer);

        llvm::BasicBlock *check_kind = llvm::BasicBlock::Create(*context_, "iterate.kind", function_);
        llvm::BasicBlock *array_path = llvm::BasicBlock::Create(*context_, "iterate.array", function_);
        llvm::BasicBlock *array_nonnull = llvm::BasicBlock::Create(*context_, "iterate.array.nonnull", function_);
        llvm::BasicBlock *array_version_ok = llvm::BasicBlock::Create(*context_, "iterate.array.version", function_);
        llvm::BasicBlock *array_item = llvm::BasicBlock::Create(*context_, "iterate.array.item", function_);
        llvm::BasicBlock *array_done = llvm::BasicBlock::Create(*context_, "iterate.array.done", function_);
        llvm::BasicBlock *array_merge = llvm::BasicBlock::Create(*context_, "iterate.array.merge", function_);
        llvm::BasicBlock *mutated = llvm::BasicBlock::Create(*context_, "iterate.mutated", function_);
        llvm::BasicBlock *slow = llvm::BasicBlock::Create(*context_, "iterate.slow", function_);
        llvm::BasicBlock *merge = llvm::BasicBlock::Create(*context_, "iterate.merge", function_);
        builder.CreateCondBr(valid, check_kind, slow);

        builder.SetInsertPoint(check_kind);
        Value *iterator_pointer = builder.CreateIntToPtr(payload, ptr_, "iterator.ptr");
        Value *kind = builder.CreateAlignedLoad(
                i8_, builder.CreateConstGEP1_64(i8_, iterator_pointer, offsetof(GcIterator, kind)), llvm::Align(1),
                "iterator.kind");
        Value *is_array = builder.CreateICmpEQ(
                kind, llvm::ConstantInt::get(i8_, static_cast<std::uint8_t>(GcIteratorKind::Array)));
        builder.CreateCondBr(is_array, array_path, slow);

        builder.SetInsertPoint(array_path);
        builder.CreateAlignedStore(llvm::ConstantInt::getFalse(*context_),
                                   builder.CreateConstGEP1_64(i8_, iterator_pointer, offsetof(GcIterator, has_current)),
                                   llvm::Align(1));
        builder.CreateAlignedStore(boxed_constant(JsValue::make_undefined()),
                                   builder.CreateConstGEP1_64(i8_, iterator_pointer, offsetof(GcIterator, current_key)),
                                   llvm::Align(16));
        builder.CreateAlignedStore(
                boxed_constant(JsValue::make_undefined()),
                builder.CreateConstGEP1_64(i8_, iterator_pointer, offsetof(GcIterator, current_value)),
                llvm::Align(16));
        Value *array_pointer = builder.CreateAlignedLoad(
                ptr_, builder.CreateConstGEP1_64(i8_, iterator_pointer, offsetof(GcIterator, array)), llvm::Align(8),
                "iterator.array");
        builder.CreateCondBr(builder.CreateIsNotNull(array_pointer), array_nonnull, array_done);

        builder.SetInsertPoint(array_nonnull);
        Value *array_version = builder.CreateAlignedLoad(
                i64_, builder.CreateConstGEP1_64(i8_, array_pointer, offsetof(GcArray, version)), llvm::Align(8),
                "array.version");
        Value *expected_version = builder.CreateAlignedLoad(
                i64_, builder.CreateConstGEP1_64(i8_, iterator_pointer, offsetof(GcIterator, expected_version)),
                llvm::Align(8), "iterator.version");
        builder.CreateCondBr(builder.CreateICmpEQ(array_version, expected_version), array_version_ok, mutated);

        builder.SetInsertPoint(array_version_ok);
        Value *index = builder.CreateAlignedLoad(
                i64_, builder.CreateConstGEP1_64(i8_, iterator_pointer, offsetof(GcIterator, index)), llvm::Align(8),
                "iterator.index");
        Value *size =
                builder.CreateAlignedLoad(i64_, builder.CreateConstGEP1_64(i8_, array_pointer, offsetof(GcArray, size)),
                                          llvm::Align(8), "array.size");
        builder.CreateCondBr(builder.CreateICmpULT(index, size), array_item, array_done);

        builder.SetInsertPoint(array_item);
        Value *elements = builder.CreateAlignedLoad(
                ptr_, builder.CreateConstGEP1_64(i8_, array_pointer, offsetof(GcArray, elems)), llvm::Align(8),
                "array.elements");
        Value *element_pointer = builder.CreateGEP(i128_, elements, index);
        Value *element = builder.CreateAlignedLoad(i128_, element_pointer, llvm::Align(16), "iterator.value");
        builder.CreateAlignedStore(
                element, builder.CreateConstGEP1_64(i8_, iterator_pointer, offsetof(GcIterator, current_value)),
                llvm::Align(16));
        builder.CreateAlignedStore(box_payload(builder, index, JsTag::Int64),
                                   builder.CreateConstGEP1_64(i8_, iterator_pointer, offsetof(GcIterator, current_key)),
                                   llvm::Align(16));
        builder.CreateAlignedStore(llvm::ConstantInt::getTrue(*context_),
                                   builder.CreateConstGEP1_64(i8_, iterator_pointer, offsetof(GcIterator, has_current)),
                                   llvm::Align(1));
        builder.CreateAlignedStore(builder.CreateAdd(index, constant_i64(1)),
                                   builder.CreateConstGEP1_64(i8_, iterator_pointer, offsetof(GcIterator, index)),
                                   llvm::Align(8));
        builder.CreateBr(array_merge);
        llvm::BasicBlock *item_predecessor = builder.GetInsertBlock();

        builder.SetInsertPoint(array_done);
        builder.CreateBr(array_merge);
        llvm::BasicBlock *done_predecessor = builder.GetInsertBlock();

        builder.SetInsertPoint(array_merge);
        auto *array_has_item = builder.CreatePHI(i1_, 2, "iterator.has_item");
        array_has_item->addIncoming(llvm::ConstantInt::getTrue(*context_), item_predecessor);
        array_has_item->addIncoming(llvm::ConstantInt::getFalse(*context_), done_predecessor);
        Value *array_result = box_boolean(builder, array_has_item, "iterator.result");
        builder.CreateBr(merge);
        llvm::BasicBlock *array_predecessor = builder.GetInsertBlock();

        builder.SetInsertPoint(mutated);
        Value *mutation_exception = boxed_constant(JsValue::make_exception(ExceptionKind::IterationError));
        if (instruction.exception_target != kInvalidBlock) {
            builder.CreateBr(llvm_blocks_[instruction.exception_target]);
            record_edge(block.id, instruction.exception_target, EdgeKind::Exception, mutated,
                        {{instruction.exception, mutation_exception}});
        } else {
            store_frame_boxed(builder, offsetof(run::JitFrameHeader, pending_exception), mutation_exception);
            emit_return(builder, run::JitStatus::Exception);
        }

        builder.SetInsertPoint(slow);
        builder.CreateAlignedStore(iterator_boxed, argument_pointer(builder, 0), llvm::Align(16));
        store_frame_i32(builder, offsetof(run::JitFrameHeader, active_pc), instruction.pc);
        Value *status =
                builder.CreateCall(iterate_next_call_, {frame_, argument_scratch_, out_scratch_}, "iterate.status");
        Value *slow_result = builder.CreateAlignedLoad(i128_, out_scratch_, llvm::Align(16), "iterate.out");
        if (instruction.exception != kInvalidValue) {
            versions[instruction.exception] = slow_result;
            base_values_[instruction.exception] = slow_result;
        }
        llvm::BasicBlock *slow_success = nullptr;
        if (!emit_exact_status_paths(builder, block, instruction, status, slow_result, merge, false, {},
                                     &slow_success)) {
            return false;
        }

        builder.SetInsertPoint(merge);
        auto *result = builder.CreatePHI(i128_, 2, "iterate.result");
        result->addIncoming(array_result, array_predecessor);
        result->addIncoming(slow_result, slow_success);
        versions[instruction.result] = result;
        base_values_[instruction.result] = result;
        builder.CreateBr(llvm_blocks_[instruction.normal_target]);
        record_edge(block.id, instruction.normal_target, EdgeKind::Normal, merge);
        return true;
    }

    Value *emit_iterator_value(IRBuilder<> &builder, Value *iterator_boxed, bool key) {
        Value *tag = boxed_tag(builder, iterator_boxed);
        Value *subtag_bits = builder.CreateLShr(iterator_boxed, llvm::ConstantInt::get(i128_, 120));
        Value *subtag = builder.CreateTrunc(subtag_bits, i8_);
        Value *payload = boxed_payload(builder, iterator_boxed);
        Value *valid = builder.CreateAnd(
                builder.CreateAnd(
                        builder.CreateICmpEQ(tag,
                                             llvm::ConstantInt::get(i8_, static_cast<std::uint8_t>(JsTag::HeapRef))),
                        builder.CreateICmpEQ(
                                subtag, llvm::ConstantInt::get(i8_, static_cast<std::uint8_t>(GcHeapKind::Iterator)))),
                builder.CreateICmpNE(payload, constant_i64(0)));

        llvm::BasicBlock *check_current = llvm::BasicBlock::Create(*context_, "iterator.current", function_);
        llvm::BasicBlock *load_current = llvm::BasicBlock::Create(*context_, "iterator.load", function_);
        llvm::BasicBlock *missing = llvm::BasicBlock::Create(*context_, "iterator.missing", function_);
        llvm::BasicBlock *merge = llvm::BasicBlock::Create(*context_, "iterator.value.merge", function_);
        builder.CreateCondBr(valid, check_current, missing);

        builder.SetInsertPoint(check_current);
        Value *iterator_pointer = builder.CreateIntToPtr(payload, ptr_);
        Value *has_current = builder.CreateAlignedLoad(
                i8_, builder.CreateConstGEP1_64(i8_, iterator_pointer, offsetof(GcIterator, has_current)),
                llvm::Align(1));
        builder.CreateCondBr(builder.CreateICmpNE(has_current, llvm::ConstantInt::get(i8_, 0)), load_current, missing);

        builder.SetInsertPoint(load_current);
        std::size_t offset = key ? offsetof(GcIterator, current_key) : offsetof(GcIterator, current_value);
        Value *current = builder.CreateAlignedLoad(i128_, builder.CreateConstGEP1_64(i8_, iterator_pointer, offset),
                                                   llvm::Align(16), key ? "iterator.key" : "iterator.value");
        builder.CreateBr(merge);
        llvm::BasicBlock *current_predecessor = builder.GetInsertBlock();

        builder.SetInsertPoint(missing);
        builder.CreateBr(merge);
        llvm::BasicBlock *missing_predecessor = builder.GetInsertBlock();

        builder.SetInsertPoint(merge);
        auto *result = builder.CreatePHI(i128_, 2, key ? "iterator.key.result" : "iterator.value.result");
        result->addIncoming(current, current_predecessor);
        result->addIncoming(boxed_constant(JsValue::make_undefined()), missing_predecessor);
        return result;
    }

    Value *emit_truth(IRBuilder<> &builder, Value *boxed, std::uint16_t type_mask) {
        Value *payload = boxed_payload(builder, boxed, "truth.payload");
        if (type_mask == TypeUndefined || type_mask == TypeNull) {
            return llvm::ConstantInt::getFalse(*context_);
        }
        if (type_mask == TypeBoolean || type_mask == TypeInteger) {
            return builder.CreateICmpNE(payload, constant_i64(0), "truth.immediate");
        }
        if (type_mask == TypeFloat) {
            Value *number = builder.CreateBitCast(payload, double_);
            return builder.CreateFCmpONE(number, llvm::ConstantFP::get(double_, 0.0), "truth.float");
        }
        if (type_mask == TypeBorrowedBinary || type_mask == TypeException) {
            return llvm::ConstantInt::getTrue(*context_);
        }
        if (type_mask == TypeBorrowedString) {
            Value *length_bits = builder.CreateLShr(boxed, llvm::ConstantInt::get(i128_, 64));
            Value *length = builder.CreateTrunc(length_bits, i32_);
            return builder.CreateICmpNE(length, constant_i32(0), "truth.string");
        }

        llvm::BasicBlock *simple = llvm::BasicBlock::Create(*context_, "truth.simple", function_);
        llvm::BasicBlock *fallback = llvm::BasicBlock::Create(*context_, "truth.slow", function_);
        llvm::BasicBlock *merge = llvm::BasicBlock::Create(*context_, "truth.merge", function_);
        Value *tag = boxed_tag(builder, boxed, "truth.tag");
        Value *is_simple =
                builder.CreateICmpULE(tag, llvm::ConstantInt::get(i8_, static_cast<std::uint8_t>(JsTag::Double)));
        builder.CreateCondBr(is_simple, simple, fallback);

        builder.SetInsertPoint(simple);
        Value *number = builder.CreateBitCast(payload, double_);
        Value *float_truth = builder.CreateFCmpONE(number, llvm::ConstantFP::get(double_, 0.0));
        Value *payload_truth = builder.CreateICmpNE(payload, constant_i64(0));
        Value *is_double =
                builder.CreateICmpEQ(tag, llvm::ConstantInt::get(i8_, static_cast<std::uint8_t>(JsTag::Double)));
        Value *numeric_truth = builder.CreateSelect(is_double, float_truth, payload_truth);
        Value *has_value =
                builder.CreateICmpUGT(tag, llvm::ConstantInt::get(i8_, static_cast<std::uint8_t>(JsTag::Null)));
        Value *simple_truth = builder.CreateAnd(has_value, numeric_truth);
        builder.CreateBr(merge);
        llvm::BasicBlock *simple_predecessor = builder.GetInsertBlock();

        builder.SetInsertPoint(fallback);
        builder.CreateAlignedStore(boxed, argument_pointer(builder, 0), llvm::Align(16));
        Value *slow_truth =
                builder.CreateICmpNE(builder.CreateCall(logic_call_, {argument_pointer(builder, 0)}), constant_i32(0));
        builder.CreateBr(merge);
        llvm::BasicBlock *fallback_predecessor = builder.GetInsertBlock();

        builder.SetInsertPoint(merge);
        auto *truth = builder.CreatePHI(i1_, 2, "truth");
        truth->addIncoming(simple_truth, simple_predecessor);
        truth->addIncoming(slow_truth, fallback_predecessor);
        return truth;
    }

    bool emit_runtime_instruction(IRBuilder<> &builder, const BasicBlock &block, std::uint32_t instruction_index,
                                  const Instruction &instruction, std::vector<Value *> &versions) {
        if (!has_effect(instruction.effects, Effect::MayGC)) {
            return fail(JitCompileStage::LlvmIr, "NoGC opcode reached generic statepoint lowering", instruction.pc);
        }
        store_arguments(builder, instruction, versions);
        if (!error_.message.empty()) {
            return false;
        }
        store_frame_i32(builder, offsetof(run::JitFrameHeader, active_pc), instruction.pc);
        const AsyncSiteSpill *async_site = spill_.site(block.id, instruction_index);
        if (has_effect(instruction.effects, Effect::MaySuspend)) {
            if (!async_site) {
                return fail(JitCompileStage::LlvmIr, "async instruction has no resume descriptor", instruction.pc);
            }
            for (ValueId id: async_site->values) {
                std::uint32_t slot = persistent_slot_[id];
                Value *value = version_for(versions, id, instruction.pc);
                if (slot == kInvalidSlot || !value) {
                    return fail(JitCompileStage::LlvmIr, "invalid async persistent value", instruction.pc);
                }
                store_persistent(builder, slot, value);
            }
            store_frame_i32(builder, offsetof(run::JitFrameHeader, resume_id), async_site->resume_id);
        }

        Value *status = emit_generic_statepoint_call(builder, block, instruction_index, instruction, versions);
        if (!status) {
            return false;
        }
        Value *out = builder.CreateAlignedLoad(i128_, out_scratch_, llvm::Align(16), "runtime.out");
        if (instruction.result != kInvalidValue) {
            versions[instruction.result] = out;
            base_values_[instruction.result] = out;
        }
        if (instruction.exception != kInvalidValue) {
            versions[instruction.exception] = out;
            if (instruction.exception != instruction.result) {
                base_values_[instruction.exception] = out;
            }
        }

        llvm::BasicBlock *success = llvm::BasicBlock::Create(*context_, "status.success", function_);
        llvm::BasicBlock *exception = llvm::BasicBlock::Create(*context_, "status.exception", function_);
        llvm::BasicBlock *abort = llvm::BasicBlock::Create(*context_, "status.abort", function_);
        llvm::BasicBlock *suspend = llvm::BasicBlock::Create(*context_, "status.suspend", function_);
        llvm::BasicBlock *invalid = llvm::BasicBlock::Create(*context_, "status.invalid", function_);
        auto *status_switch = builder.CreateSwitch(status, invalid, 4);
        status_switch->addCase(constant_i32(static_cast<std::uint32_t>(run::JitStatus::Success)), success);
        status_switch->addCase(constant_i32(static_cast<std::uint32_t>(run::JitStatus::Exception)), exception);
        status_switch->addCase(constant_i32(static_cast<std::uint32_t>(run::JitStatus::Abort)), abort);
        status_switch->addCase(constant_i32(static_cast<std::uint32_t>(run::JitStatus::Suspend)), suspend);

        builder.SetInsertPoint(success);
        if (has_effect(instruction.effects, Effect::MaySuspend)) {
            store_frame_i32(builder, offsetof(run::JitFrameHeader, resume_id), 0);
        }
        if (instruction.normal_target == kInvalidBlock) {
            return fail(JitCompileStage::LlvmIr, "runtime instruction has no normal continuation", instruction.pc);
        }
        builder.CreateBr(llvm_blocks_[instruction.normal_target]);
        record_edge(block.id, instruction.normal_target, EdgeKind::Normal, success);

        builder.SetInsertPoint(exception);
        if (has_effect(instruction.effects, Effect::MaySuspend)) {
            store_frame_i32(builder, offsetof(run::JitFrameHeader, resume_id), 0);
        }
        if (instruction.exception_target != kInvalidBlock) {
            builder.CreateBr(llvm_blocks_[instruction.exception_target]);
            record_edge(block.id, instruction.exception_target, EdgeKind::Exception, exception);
        } else {
            store_frame_boxed(builder, offsetof(run::JitFrameHeader, pending_exception), out);
            emit_return(builder, run::JitStatus::Exception);
        }

        builder.SetInsertPoint(abort);
        emit_return(builder, run::JitStatus::Abort);

        builder.SetInsertPoint(suspend);
        if (has_effect(instruction.effects, Effect::MaySuspend)) {
            emit_return(builder, run::JitStatus::Suspend);
        } else {
            emit_abort(builder, ScriptAbortReason::Internal);
            emit_return(builder, run::JitStatus::Abort);
        }

        builder.SetInsertPoint(invalid);
        emit_abort(builder, ScriptAbortReason::Internal);
        emit_return(builder, run::JitStatus::Abort);
        return true;
    }

    bool emit_cfg_blocks() {
        for (const BasicBlock &block: cfg_.blocks()) {
            if (!block.reachable) {
                continue;
            }
            IRBuilder<> builder(llvm_blocks_[block.id]);
            auto &versions = exit_versions_[block.id];
            versions[cfg_.undefined_value()] = base_values_[cfg_.undefined_value()];
            for (const Phi &phi: block.phis) {
                versions[phi.result] = base_values_[phi.result];
            }
            bool terminated = false;
            for (std::uint32_t instruction_index = 0; instruction_index < block.instructions.size();
                 ++instruction_index) {
                const Instruction &instruction = block.instructions[instruction_index];
                std::uint8_t opcode = instruction.opcode;
                if (opcode == ir::Code::LOAD_CONST) {
                    Value *value = boxed_constant(compiled_.constant(instruction.raw >> 8u));
                    versions[instruction.result] = value;
                    base_values_[instruction.result] = value;
                } else if (opcode == ir::Code::LOAD_ROOT) {
                    Value *root_pointer =
                            builder.CreateAlignedLoad(ptr_, frame_field(builder, offsetof(run::JitFrameHeader, root)),
                                                      llvm::Align(8), "root.ptr");
                    Value *value = builder.CreateAlignedLoad(i128_, root_pointer, llvm::Align(16), "root");
                    versions[instruction.result] = value;
                    base_values_[instruction.result] = value;
                } else if (opcode == ir::Code::ITERATE_KEY || opcode == ir::Code::ITERATE_VALUE) {
                    Value *iterator = version_for(versions, instruction.operands[0], instruction.pc);
                    if (!iterator) {
                        return false;
                    }
                    Value *value = emit_iterator_value(builder, iterator, opcode == ir::Code::ITERATE_KEY);
                    versions[instruction.result] = value;
                    base_values_[instruction.result] = value;
                } else if (opcode >= ir::Code::BOP_PLUS && opcode <= ir::Code::BOP_IN) {
                    if (!emit_binary_instruction(builder, block, instruction_index, instruction, versions)) {
                        return false;
                    }
                    terminated = true;
                } else if (opcode >= ir::Code::UNARY_PLUS && opcode <= ir::Code::UNARY_TYPEOF) {
                    if (!emit_unary_instruction(builder, block, instruction, versions)) {
                        return false;
                    }
                    terminated = true;
                } else if (opcode == ir::Code::ITERATE_NEXT) {
                    if (!emit_iterate_next_instruction(builder, block, instruction, versions)) {
                        return false;
                    }
                    terminated = true;
                } else if (opcode == ir::Code::JUMP_IF_FALSE || opcode == ir::Code::JUMP_IF_TRUE) {
                    Value *condition = version_for(versions, instruction.operands[0], instruction.pc);
                    if (!condition || instruction.branch_target == kInvalidBlock) {
                        return fail(JitCompileStage::LlvmIr, "invalid conditional branch", instruction.pc);
                    }
                    Value *truth = emit_truth(builder, condition, cfg_.values()[instruction.operands[0]].type_mask);
                    BlockId fallthrough = kInvalidBlock;
                    for (const Edge &edge: block.successors) {
                        if (edge.kind == EdgeKind::Normal && edge.successor != instruction.branch_target) {
                            fallthrough = edge.successor;
                        }
                    }
                    if (fallthrough == kInvalidBlock) {
                        return fail(JitCompileStage::LlvmIr, "conditional branch has no fallthrough", instruction.pc);
                    }
                    llvm::BasicBlock *true_target = opcode == ir::Code::JUMP_IF_TRUE
                                                            ? llvm_blocks_[instruction.branch_target]
                                                            : llvm_blocks_[fallthrough];
                    llvm::BasicBlock *false_target = opcode == ir::Code::JUMP_IF_TRUE
                                                             ? llvm_blocks_[fallthrough]
                                                             : llvm_blocks_[instruction.branch_target];
                    builder.CreateCondBr(truth, true_target, false_target);
                    record_edge(block.id, instruction.branch_target, EdgeKind::Normal, builder.GetInsertBlock());
                    record_edge(block.id, fallthrough, EdgeKind::Normal, builder.GetInsertBlock());
                    terminated = true;
                } else if (opcode == ir::Code::JUMP) {
                    if (instruction.branch_target == kInvalidBlock) {
                        return fail(JitCompileStage::LlvmIr, "invalid jump", instruction.pc);
                    }
                    builder.CreateBr(llvm_blocks_[instruction.branch_target]);
                    record_edge(block.id, instruction.branch_target, EdgeKind::Normal, builder.GetInsertBlock());
                    terminated = true;
                } else if (opcode == ir::Code::END_RETURN) {
                    if (instruction.operands.empty()) {
                        emit_return(builder, run::JitStatus::SuccessVoid);
                    } else {
                        Value *value = version_for(versions, instruction.operands[0], instruction.pc);
                        if (!value) {
                            return false;
                        }
                        store_frame_boxed(builder, offsetof(run::JitFrameHeader, pending_value), value);
                        emit_return(builder, run::JitStatus::Success);
                    }
                    terminated = true;
                } else if (opcode == ir::Code::THROW_EXP) {
                    Value *exception_value = version_for(versions, instruction.operands[0], instruction.pc);
                    if (!exception_value) {
                        return false;
                    }
                    versions[instruction.exception] = exception_value;
                    if (instruction.exception_target != kInvalidBlock) {
                        builder.CreateBr(llvm_blocks_[instruction.exception_target]);
                        record_edge(block.id, instruction.exception_target, EdgeKind::Exception,
                                    builder.GetInsertBlock());
                    } else {
                        store_frame_boxed(builder, offsetof(run::JitFrameHeader, pending_exception), exception_value);
                        emit_return(builder, run::JitStatus::Exception);
                    }
                    terminated = true;
                } else {
                    if (!emit_runtime_instruction(builder, block, instruction_index, instruction, versions)) {
                        return false;
                    }
                    terminated = true;
                }
                if (terminated) {
                    break;
                }
            }
            if (!terminated) {
                BlockId target = kInvalidBlock;
                for (const Edge &edge: block.successors) {
                    if (edge.kind == EdgeKind::Normal) {
                        if (target != kInvalidBlock) {
                            return fail(JitCompileStage::LlvmIr, "unterminated block has multiple successors",
                                        block.end_pc - 1u);
                        }
                        target = edge.successor;
                    }
                }
                if (target == kInvalidBlock) {
                    return fail(JitCompileStage::LlvmIr, "unterminated block has no successor", block.end_pc - 1u);
                }
                builder.CreateBr(llvm_blocks_[target]);
                record_edge(block.id, target, EdgeKind::Normal, builder.GetInsertBlock());
            }
        }
        return true;
    }

    const PhiIncoming *phi_incoming(const Phi &phi, BlockId predecessor) const noexcept {
        auto it = std::ranges::find_if(
                phi.incoming, [&](const PhiIncoming &incoming) { return incoming.predecessor == predecessor; });
        return it == phi.incoming.end() ? nullptr : &*it;
    }

    bool add_resume_phi_inputs(IRBuilder<> &builder, const AsyncSiteSpill &site, BlockId successor, EdgeKind kind,
                               llvm::BasicBlock *actual_predecessor, Value *async_result, Value *async_exception) {
        const BasicBlock &target = cfg_.blocks()[successor];
        for (const Phi &phi: target.phis) {
            Value *value = nullptr;
            if (phi.kind == PhiKind::Exception) {
                if (kind != EdgeKind::Exception) {
                    continue;
                }
                value = async_exception;
            } else if (phi.kind == PhiKind::Stack && kind == EdgeKind::Exception) {
                continue;
            } else {
                const PhiIncoming *incoming = phi_incoming(phi, site.block);
                if (!incoming) {
                    return fail(JitCompileStage::LlvmIr, "resume edge has no SSA phi input");
                }
                const Instruction &async_instruction = cfg_.blocks()[site.block].instructions[site.instruction_index];
                if (incoming->value == async_instruction.result) {
                    value = async_result;
                } else if (incoming->value == cfg_.undefined_value()) {
                    value = base_values_[cfg_.undefined_value()];
                } else {
                    std::uint32_t slot = persistent_slot_[incoming->value];
                    if (slot == kInvalidSlot) {
                        // Entry phis are intentionally eager. A phi that is dead in the
                        // continuation has no async slot but still needs a verifier-valid
                        // incoming value for the synthetic resume predecessor.
                        value = base_values_[cfg_.undefined_value()];
                    } else {
                        value = load_persistent(builder, slot, "resume.value");
                    }
                }
            }
            llvm::cast<llvm::PHINode>(base_values_[phi.result])->addIncoming(value, actual_predecessor);
        }
        return true;
    }

    bool emit_resume_blocks() {
        for (std::size_t site_index = 0; site_index < spill_.sites().size(); ++site_index) {
            const AsyncSiteSpill &site = spill_.sites()[site_index];
            const Instruction &instruction = cfg_.blocks()[site.block].instructions[site.instruction_index];
            llvm::BasicBlock *resume = resume_blocks_[site_index];
            llvm::BasicBlock *value_block = llvm::BasicBlock::Create(*context_, "resume.value", function_);
            llvm::BasicBlock *exception_block = llvm::BasicBlock::Create(*context_, "resume.exception", function_);
            llvm::BasicBlock *abort_block = llvm::BasicBlock::Create(*context_, "resume.abort", function_);
            llvm::BasicBlock *invalid_block = llvm::BasicBlock::Create(*context_, "resume.invalid", function_);
            IRBuilder<> builder(resume);
            store_frame_i32(builder, offsetof(run::JitFrameHeader, resume_id), 0);
            Value *state = load_frame_i32(builder, offsetof(run::JitFrameHeader, state), "async.state");
            auto *state_switch = builder.CreateSwitch(state, invalid_block, 3);
            state_switch->addCase(constant_i32(static_cast<std::uint32_t>(run::JitRunState::AsyncValue)), value_block);
            state_switch->addCase(constant_i32(static_cast<std::uint32_t>(run::JitRunState::AsyncException)),
                                  exception_block);
            state_switch->addCase(constant_i32(static_cast<std::uint32_t>(run::JitRunState::AsyncAbort)), abort_block);

            builder.SetInsertPoint(value_block);
            store_state(builder, run::JitRunState::Running);
            Value *async_value = load_frame_boxed(builder, offsetof(run::JitFrameHeader, pending_value), "async.value");
            if (instruction.normal_target == kInvalidBlock ||
                !add_resume_phi_inputs(builder, site, instruction.normal_target, EdgeKind::Normal, value_block,
                                       async_value, nullptr)) {
                return false;
            }
            builder.CreateBr(llvm_blocks_[instruction.normal_target]);

            builder.SetInsertPoint(exception_block);
            store_state(builder, run::JitRunState::Running);
            Value *async_exception =
                    load_frame_boxed(builder, offsetof(run::JitFrameHeader, pending_exception), "async.exception");
            if (instruction.exception_target != kInvalidBlock) {
                if (!add_resume_phi_inputs(builder, site, instruction.exception_target, EdgeKind::Exception,
                                           exception_block, nullptr, async_exception)) {
                    return false;
                }
                builder.CreateBr(llvm_blocks_[instruction.exception_target]);
            } else {
                emit_return(builder, run::JitStatus::Exception);
            }

            builder.SetInsertPoint(abort_block);
            fill_abort_position(builder, instruction.pc);
            emit_return(builder, run::JitStatus::Abort);

            builder.SetInsertPoint(invalid_block);
            emit_abort(builder, ScriptAbortReason::InvalidState);
            emit_return(builder, run::JitStatus::Abort);
        }
        return true;
    }

    bool fill_cfg_phis() {
        for (const LoweredEdge &edge: lowered_edges_) {
            const BasicBlock &successor = cfg_.blocks()[edge.successor];
            for (const Phi &phi: successor.phis) {
                if (phi.kind == PhiKind::Exception && edge.kind != EdgeKind::Exception) {
                    continue;
                }
                if (phi.kind == PhiKind::Stack && edge.kind == EdgeKind::Exception) {
                    continue;
                }
                const PhiIncoming *incoming = phi_incoming(phi, edge.predecessor);
                if (!incoming) {
                    return fail(JitCompileStage::LlvmIr, "CFG edge has no phi input", successor.start_pc);
                }
                Value *value = nullptr;
                auto override = std::ranges::find_if(edge.value_overrides,
                                                     [&](const auto &item) { return item.first == incoming->value; });
                if (override != edge.value_overrides.end()) {
                    value = override->second;
                } else {
                    value = exit_versions_[edge.predecessor][incoming->value];
                }
                if (!value) {
                    value = base_values_[incoming->value];
                }
                if (!value) {
                    return fail(JitCompileStage::LlvmIr, "phi input has no lowered value", successor.start_pc);
                }
                llvm::cast<llvm::PHINode>(base_values_[phi.result])->addIncoming(value, edge.llvm_predecessor);
            }
        }
        return true;
    }
};

} // namespace

bool jit_is_available() noexcept { return static_cast<bool>(engine_holder().state); }

std::expected<std::shared_ptr<const run::JitCode>, JitCompileError>
compile_jit(std::shared_ptr<const ir::Compiled> compiled) {
    if (!compiled) {
        return std::unexpected(make_error(JitCompileStage::Cfg, "compiled script is null"));
    }
    EngineHolder &holder = engine_holder();
    if (!holder.state) {
        return std::unexpected(make_error(JitCompileStage::Unavailable, holder.error));
    }
    if (compiled->code_size() > kMaxJitCodeCount) {
        return std::unexpected(make_error(JitCompileStage::Cfg, "bytecode exceeds JIT size limit"));
    }
    auto cfg = Cfg::build(*compiled);
    if (!cfg) {
        return std::unexpected(make_error(JitCompileStage::Cfg, cfg.error().message, cfg.error().pc));
    }
    if (cfg->values().size() > kMaxJitValueCount) {
        return std::unexpected(make_error(JitCompileStage::Cfg, "SSA value count exceeds JIT size limit"));
    }
    if (cfg->blocks().size() > kMaxJitBlockCount) {
        return std::unexpected(make_error(JitCompileStage::Cfg, "basic-block count exceeds JIT size limit"));
    }
    std::size_t edge_count = 0;
    for (const BasicBlock &block: cfg->blocks()) {
        edge_count += block.successors.size();
    }
    if (edge_count > kMaxJitEdgeCount) {
        return std::unexpected(make_error(JitCompileStage::Cfg, "CFG edge count exceeds JIT size limit"));
    }
    Liveness liveness = Liveness::analyze(*cfg);
    AsyncSpill spill = AsyncSpill::analyze(*cfg, liveness);
    if (spill.persistent_values().size() > kMaxJitAsyncSlots) {
        return std::unexpected(make_error(JitCompileStage::Cfg, "async frame exceeds JIT size limit"));
    }

    std::uint64_t module_id = g_module_id.fetch_add(1, std::memory_order_relaxed);
    std::string entry_name = "fiber_script_entry_" + std::to_string(module_id);
    ModuleLowerer lowerer(*compiled, *cfg, liveness, spill, holder.state->jit->getDataLayout(),
                          holder.state->jit->getTargetTriple().str(), entry_name);
    auto lowered = lowerer.lower();
    if (!lowered) {
        return std::unexpected(std::move(lowered.error()));
    }

    std::lock_guard compile_lock(holder.state->compile_mutex);
    llvm::orc::ResourceTrackerSP tracker = holder.state->jit->getMainJITDylib().createResourceTracker();
    llvm::orc::ResourceKey key = 0;
    if (llvm::Error key_error = tracker->withResourceKeyDo([&](llvm::orc::ResourceKey value) { key = value; })) {
        return std::unexpected(make_error(JitCompileStage::OrcAdd, llvm_error_string(std::move(key_error))));
    }
    auto stack_maps = std::make_shared<run::NativeStackMapTable>();
    holder.state->stack_maps->bind(key, stack_maps);
    llvm::orc::ThreadSafeModule thread_safe_module(std::move(lowered->module), std::move(lowered->context));
    if (llvm::Error add_error = holder.state->jit->addIRModule(tracker, std::move(thread_safe_module))) {
        llvm::consumeError(tracker->remove());
        return std::unexpected(make_error(JitCompileStage::OrcAdd, llvm_error_string(std::move(add_error))));
    }
    auto symbol = holder.state->jit->lookup(lowered->entry_name);
    if (!symbol) {
        std::string message = llvm_error_string(symbol.takeError());
        llvm::consumeError(tracker->remove());
        return std::unexpected(make_error(JitCompileStage::Lookup, std::move(message)));
    }
    if (stack_maps->size() != lowered->statepoint_count) {
        std::string message = "linked stack-map record count " + std::to_string(stack_maps->size()) +
                              " does not match statepoint count " + std::to_string(lowered->statepoint_count);
        llvm::consumeError(tracker->remove());
        return std::unexpected(make_error(JitCompileStage::StackMap, std::move(message)));
    }
    auto owner = std::make_shared<ResourceOwner>();
    owner->engine = holder.state;
    owner->tracker = std::move(tracker);
    run::JitEntry entry = symbol->toPtr<run::JitEntry>();
    auto code = std::make_shared<run::JitCode>(entry, std::move(compiled), lowered->async_value_count,
                                               std::move(stack_maps), std::move(owner));
    return std::static_pointer_cast<const run::JitCode>(std::move(code));
}

} // namespace fiber::script::jit

#endif
