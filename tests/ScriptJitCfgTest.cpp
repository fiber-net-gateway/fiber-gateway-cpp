#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include <fiber/script/AsyncTask.h>
#include <fiber/script/Library.h>
#include <fiber/script/ir/Code.h>
#include <fiber/script/ir/Compiler.h>
#include <fiber/script/jit/Cfg.h>
#include <fiber/script/parse/Parser.h>

#if FIBER_ENABLE_SCRIPT_JIT
#include <fiber/script/jit/JitCompiler.h>
#include <fiber/script/run/JitCode.h>
#endif

namespace {

using fiber::script::Library;
using fiber::script::jit::Cfg;

fiber::script::AsyncTask async_identity(void *userdata, const Library::HostCallFrame &frame,
                                        Library::Arguments arguments) noexcept {
    (void) userdata;
    (void) frame;
    fiber::script::JsValue value = arguments.argc == 0 ? fiber::script::JsValue::make_undefined() : arguments.args[0];
    co_return fiber::script::AbiResult::success(value);
}

class TestLibrary final : public Library {
public:
    TestLibrary() {
        async_.kind = HostCallable::Kind::AsyncFunction;
        async_.async_function = &async_identity;
        async_.debug_name = "asyncIdentity";
    }

    FunctionMatchResult resolve_func(std::string_view name, const FunctionMatchRequest &request) const override {
        (void) name;
        (void) request;
        return FunctionMatchResult::not_found();
    }

    FunctionMatchResult resolve_async_func(std::string_view name, const FunctionMatchRequest &request) const override {
        if (name != "asyncIdentity" || request.has_spread || request.known_argc != 1) {
            return FunctionMatchResult::not_found();
        }
        FunctionSignature signature;
        signature.required_argc = 1;
        signature.fixed_argc = 1;
        signature.variadic = false;
        return FunctionMatchResult::found(&async_, signature, nullptr, 0);
    }

    const HostCallable *resolve_constant(std::string_view namespace_name, std::string_view key) const override {
        (void) namespace_name;
        (void) key;
        return nullptr;
    }

    const HostCallable *resolve_async_constant(std::string_view namespace_name, std::string_view key) const override {
        (void) namespace_name;
        (void) key;
        return nullptr;
    }

    DirectiveDef *resolve_directive_def(std::string_view type, std::string_view name,
                                        const std::vector<fiber::script::JsValue> &literals) const override {
        (void) type;
        (void) name;
        (void) literals;
        return nullptr;
    }

private:
    HostCallable async_{};
};

fiber::script::ir::Compiled compile_script(std::string_view source, TestLibrary &library) {
    fiber::script::parse::Parser parser(library, true);
    auto parsed = parser.parse_script(source);
    EXPECT_TRUE(parsed.has_value()) << parsed.error().message;
    if (!parsed) {
        return {};
    }
    auto compiled = fiber::script::ir::Compiler::compile(*parsed.value());
    EXPECT_TRUE(compiled.has_value()) << (compiled ? "" : compiled.error().message);
    return compiled ? std::move(compiled.value()) : fiber::script::ir::Compiled{};
}

void expect_valid_phi_inputs(const Cfg &cfg) {
    for (const auto &block: cfg.blocks()) {
        if (!block.reachable) {
            continue;
        }
        for (const auto &phi: block.phis) {
            EXPECT_NE(phi.result, fiber::script::jit::kInvalidValue);
            EXPECT_FALSE(phi.incoming.empty());
            for (const auto &incoming: phi.incoming) {
                EXPECT_LT(incoming.predecessor, cfg.blocks().size());
                EXPECT_LT(incoming.value, cfg.values().size());
            }
        }
    }
}

TEST(ScriptJitCfgTest, BuildsBranchingSsaWithVariablePhi) {
    TestLibrary library;
    auto compiled = compile_script("let x = 1; if (a) { x = 2; } else { x = 3; } return x;", library);
    auto cfg = Cfg::build(compiled);
    ASSERT_TRUE(cfg.has_value()) << cfg.error().message;

    expect_valid_phi_inputs(*cfg);
    bool found_variable_merge = false;
    for (const auto &block: cfg->blocks()) {
        for (const auto &phi: block.phis) {
            if (phi.kind == fiber::script::jit::PhiKind::Variable && phi.incoming.size() >= 2) {
                found_variable_merge = true;
            }
        }
    }
    EXPECT_TRUE(found_variable_merge);
}

TEST(ScriptJitCfgTest, ModelsExceptionEdgeAndCatchValue) {
    TestLibrary library;
    auto compiled = compile_script("try { throw \"boom\"; } catch (e) { return e; }", library);
    auto cfg = Cfg::build(compiled);
    ASSERT_TRUE(cfg.has_value()) << cfg.error().message;

    expect_valid_phi_inputs(*cfg);
    bool found_exception_edge = false;
    bool found_exception_phi = false;
    for (const auto &block: cfg->blocks()) {
        found_exception_edge |= std::ranges::any_of(block.successors, [](const auto &edge) {
            return edge.kind == fiber::script::jit::EdgeKind::Exception;
        });
        found_exception_phi |= std::ranges::any_of(
                block.phis, [](const auto &phi) { return phi.kind == fiber::script::jit::PhiKind::Exception; });
    }
    EXPECT_TRUE(found_exception_edge);
    EXPECT_TRUE(found_exception_phi);
}

TEST(ScriptJitCfgTest, ComputesLoopLiveness) {
    TestLibrary library;
    auto compiled =
            compile_script("let sum = 0; for (let k, v of [10, 20, 30]) { sum = sum + v; } return sum;", library);
    auto cfg = Cfg::build(compiled);
    ASSERT_TRUE(cfg.has_value()) << cfg.error().message;

    auto liveness = fiber::script::jit::Liveness::analyze(*cfg);
    bool found_back_edge = false;
    for (const auto &block: cfg->blocks()) {
        for (const auto &edge: block.successors) {
            if (cfg->blocks()[edge.successor].start_pc <= block.start_pc) {
                found_back_edge = true;
                EXPECT_FALSE(liveness.block(block.id).live_out.values().empty());
            }
        }
    }
    EXPECT_TRUE(found_back_edge);
}

TEST(ScriptJitCfgTest, PromotesOnlyValuesLiveAcrossAsyncCall) {
    TestLibrary library;
    auto compiled = compile_script("let x = 7; let y = asyncIdentity(3); return x + y;", library);
    auto cfg = Cfg::build(compiled);
    ASSERT_TRUE(cfg.has_value()) << cfg.error().message;

    auto liveness = fiber::script::jit::Liveness::analyze(*cfg);
    auto spill = fiber::script::jit::AsyncSpill::analyze(*cfg, liveness);
    ASSERT_EQ(spill.sites().size(), 1);
    EXPECT_EQ(spill.sites()[0].resume_id, 1);
    EXPECT_EQ(spill.sites()[0].values.size(), 1);
    EXPECT_EQ(spill.persistent_values(), spill.sites()[0].values);
}

TEST(ScriptJitCfgTest, ClassifiesGcAndControlEffectsPrecisely) {
    using fiber::script::ir::Code;
    using fiber::script::jit::Effect;
    using fiber::script::jit::has_effect;
    using fiber::script::jit::opcode_effects;

    EXPECT_TRUE(has_effect(opcode_effects(Code::BOP_PLUS), Effect::MayGC));
    EXPECT_TRUE(has_effect(opcode_effects(Code::BOP_PLUS), Effect::MayThrow));
    EXPECT_FALSE(has_effect(opcode_effects(Code::BOP_MINUS), Effect::MayGC));
    EXPECT_TRUE(has_effect(opcode_effects(Code::BOP_MINUS), Effect::MayThrow));
    EXPECT_FALSE(has_effect(opcode_effects(Code::BOP_EQ), Effect::MayGC));
    EXPECT_FALSE(has_effect(opcode_effects(Code::BOP_EQ), Effect::MayThrow));
    EXPECT_FALSE(has_effect(opcode_effects(Code::UNARY_NEG), Effect::MayGC));
    EXPECT_FALSE(has_effect(opcode_effects(Code::ITERATE_NEXT), Effect::MayGC));
    EXPECT_TRUE(has_effect(opcode_effects(Code::ITERATE_NEXT), Effect::MayThrow));
    EXPECT_TRUE(has_effect(opcode_effects(Code::NEW_ARRAY), Effect::MayGC));
    EXPECT_TRUE(has_effect(opcode_effects(Code::CALL_ASYNC_FUNC), Effect::CallsHost));
    EXPECT_TRUE(has_effect(opcode_effects(Code::CALL_ASYNC_FUNC), Effect::MayGC));
    EXPECT_TRUE(has_effect(opcode_effects(Code::CALL_ASYNC_FUNC), Effect::MaySuspend));
}

TEST(ScriptJitCfgTest, RetainsPreciseSsaValueTypesAcrossPhi) {
    TestLibrary library;
    auto compiled = compile_script("let x = 1; if (a) { x = 2; } else { x = 3; } return x < 4;", library);
    auto cfg = Cfg::build(compiled);
    ASSERT_TRUE(cfg.has_value()) << cfg.error().message;

    bool found_integer_phi = false;
    bool found_boolean_result = false;
    for (const auto &block: cfg->blocks()) {
        for (const auto &phi: block.phis) {
            found_integer_phi |= cfg->values()[phi.result].type_mask == fiber::script::jit::TypeInteger;
        }
        for (const auto &instruction: block.instructions) {
            if (instruction.opcode == fiber::script::ir::Code::BOP_LT) {
                ASSERT_NE(instruction.result, fiber::script::jit::kInvalidValue);
                found_boolean_result |= cfg->values()[instruction.result].type_mask == fiber::script::jit::TypeBoolean;
            }
        }
    }
    EXPECT_TRUE(found_integer_phi);
    EXPECT_TRUE(found_boolean_result);
}

#if FIBER_ENABLE_SCRIPT_JIT

TEST(ScriptJitCfgTest, ImportsAndInlinesAuditedNoGcOperatorBitcode) {
    constexpr std::array<std::string_view, 8> sources{
            "return $ - 1.5;", "return $ * 1.5;", "return $ / 1.5;", "return $ % 1.5;",
            "return +$;",      "return -$;",      "return !$;",      "return typeof $;",
    };

    TestLibrary library;
    for (std::string_view source: sources) {
        SCOPED_TRACE(source);
        auto compiled = std::make_shared<fiber::script::ir::Compiled>(compile_script(source, library));
        auto code = fiber::script::jit::compile_jit(std::move(compiled));
        ASSERT_TRUE(code.has_value()) << code.error().message;
        EXPECT_EQ((*code)->inlined_operator_helper_count(), 1u);
    }
}

TEST(ScriptJitCfgTest, KeepsMayGcAndUnauditedHelpersAsNativeLeaves) {
    constexpr std::array<std::string_view, 2> sources{
            "return $ + \"!\";",
            "return $ < 1.5;",
    };

    TestLibrary library;
    for (std::string_view source: sources) {
        SCOPED_TRACE(source);
        auto compiled = std::make_shared<fiber::script::ir::Compiled>(compile_script(source, library));
        auto code = fiber::script::jit::compile_jit(std::move(compiled));
        ASSERT_TRUE(code.has_value()) << code.error().message;
        EXPECT_EQ((*code)->inlined_operator_helper_count(), 0u);
    }
}

#endif

} // namespace
