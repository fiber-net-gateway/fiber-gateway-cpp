#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "script/Library.h"
#include "script/ir/Code.h"
#include "script/ir/Compiler.h"
#include "script/parse/Parser.h"

namespace {

class TestLibrary final : public fiber::script::Library {
public:
    FunctionMatchResult resolve_func(std::string_view name, const FunctionMatchRequest &request) const override {
        (void) name;
        (void) request;
        return FunctionMatchResult::not_found();
    }

    FunctionMatchResult resolve_async_func(std::string_view name, const FunctionMatchRequest &request) const override {
        (void) name;
        (void) request;
        return FunctionMatchResult::not_found();
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
};

fiber::script::ScriptStatus compile_error_dummy(void *userdata, const fiber::script::Library::HostCallFrame &frame,
                                                const fiber::script::Library::Arguments &arguments,
                                                fiber::script::ValueHandle out) noexcept {
    (void) userdata;
    (void) frame;
    (void) arguments;
    *out = fiber::script::JsValue::make_undefined();
    return fiber::script::ScriptStatus::success();
}

class CompileErrorLibrary final : public fiber::script::Library {
public:
    CompileErrorLibrary() {
        func_.kind = HostCallable::Kind::SyncFunction;
        func_.function = &compile_error_dummy;
        unsupported_default_[0] = fiber::script::JsValue::make_exception(fiber::script::ExceptionKind::TypeError);
    }

    FunctionMatchResult resolve_func(std::string_view name, const FunctionMatchRequest &request) const override {
        if (name == "variadic") {
            FunctionSignature signature;
            signature.required_argc = 0;
            signature.fixed_argc = 0;
            signature.variadic = true;
            return FunctionMatchResult::found(&func_, signature, nullptr, 0);
        }
        if (name == "defaultException") {
            if (request.has_spread || request.known_argc > 1) {
                return FunctionMatchResult::arity_mismatch();
            }
            FunctionSignature signature;
            signature.required_argc = 0;
            signature.fixed_argc = 1;
            signature.variadic = false;
            const std::uint16_t default_count = request.known_argc == 0 ? 1 : 0;
            const fiber::script::JsValue *defaults = default_count == 0 ? nullptr : unsupported_default_;
            return FunctionMatchResult::found(&func_, signature, defaults, default_count);
        }
        return FunctionMatchResult::not_found();
    }

    FunctionMatchResult resolve_async_func(std::string_view name, const FunctionMatchRequest &request) const override {
        (void) name;
        (void) request;
        return FunctionMatchResult::not_found();
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
    HostCallable func_{};
    fiber::script::JsValue unsupported_default_[1]{};
};

fiber::script::ir::Compiled compile_script(std::string_view script) {
    TestLibrary library;
    fiber::script::parse::Parser parser(library, true);
    auto parsed = parser.parse_script(script);
    EXPECT_TRUE(parsed.has_value()) << parsed.error().message;
    auto compiled = fiber::script::ir::Compiler::compile(*parsed.value());
    if (!compiled) {
        ADD_FAILURE() << (compiled.error().message ? compiled.error().message : "compile failed");
        return {};
    }
    return std::move(compiled.value());
}

std::vector<std::uint8_t> extract_opcodes(const fiber::script::ir::Compiled &compiled) {
    std::vector<std::uint8_t> ops;
    ops.reserve(compiled.code_size());
    for (std::uint32_t i = 0; i < compiled.code_size(); ++i) {
        std::int32_t code = compiled.codes()[i];
        ops.push_back(static_cast<std::uint8_t>(code & 0xFF));
    }
    return ops;
}

std::size_t operand_at(const fiber::script::ir::Compiled &compiled, std::size_t index) {
    return static_cast<std::size_t>(static_cast<std::uint32_t>(compiled.codes()[index]) >> 8u);
}

} // namespace

TEST(ScriptCompilerTest, EmitsArithmeticExpressionStatement) {
    auto compiled = compile_script("1 + 2;");
    auto ops = extract_opcodes(compiled);
    ASSERT_GE(ops.size(), 6u);

    EXPECT_EQ(ops[0], fiber::script::ir::Code::LOAD_CONST);
    EXPECT_EQ(ops[1], fiber::script::ir::Code::LOAD_CONST);
    EXPECT_EQ(ops[2], fiber::script::ir::Code::BOP_PLUS);
    EXPECT_EQ(ops[3], fiber::script::ir::Code::POP);
    EXPECT_EQ(ops[4], fiber::script::ir::Code::LOAD_CONST);
    EXPECT_EQ(ops[5], fiber::script::ir::Code::END_RETURN);
}

TEST(ScriptCompilerTest, EmitsShortCircuitAnd) {
    auto compiled = compile_script("1 && 2;");
    auto ops = extract_opcodes(compiled);
    ASSERT_GE(ops.size(), 8u);

    EXPECT_EQ(ops[0], fiber::script::ir::Code::LOAD_CONST);
    EXPECT_EQ(ops[1], fiber::script::ir::Code::DUMP);
    EXPECT_EQ(ops[2], fiber::script::ir::Code::JUMP_IF_FALSE);
    EXPECT_EQ(ops[3], fiber::script::ir::Code::POP);
    EXPECT_EQ(ops[4], fiber::script::ir::Code::LOAD_CONST);
    EXPECT_EQ(ops[5], fiber::script::ir::Code::POP);
}

TEST(ScriptCompilerTest, EmitsIfElseControlFlow) {
    auto compiled = compile_script("if (1) { return 2; } else { return 3; }");
    auto ops = extract_opcodes(compiled);
    ASSERT_GE(ops.size(), 6u);

    std::size_t if_jump = 0;
    std::size_t else_jump = 0;
    int return_count = 0;
    for (std::size_t i = 0; i < ops.size(); ++i) {
        if (ops[i] == fiber::script::ir::Code::JUMP_IF_FALSE) {
            if_jump = i;
        } else if (ops[i] == fiber::script::ir::Code::JUMP) {
            else_jump = i;
        } else if (ops[i] == fiber::script::ir::Code::END_RETURN) {
            ++return_count;
        }
    }
    EXPECT_GT(return_count, 1);
    ASSERT_GT(else_jump, if_jump);

    std::size_t else_target = operand_at(compiled, if_jump);
    std::size_t end_target = operand_at(compiled, else_jump);
    EXPECT_GT(else_target, if_jump);
    EXPECT_GT(end_target, else_jump);
    EXPECT_LE(end_target, compiled.code_size());
}

TEST(ScriptCompilerTest, EmitsForeachLoopWithBreakContinue) {
    auto compiled = compile_script("for (let k, v of [1, 2]) { if (k) { continue; } break; }");
    auto ops = extract_opcodes(compiled);

    std::size_t iterate_next = 0;
    std::size_t jump_if_false = 0;
    for (std::size_t i = 0; i < ops.size(); ++i) {
        if (ops[i] == fiber::script::ir::Code::ITERATE_NEXT && iterate_next == 0) {
            iterate_next = i;
        }
        if (ops[i] == fiber::script::ir::Code::JUMP_IF_FALSE && jump_if_false == 0) {
            jump_if_false = i;
        }
    }
    ASSERT_GT(iterate_next, 0u);
    ASSERT_GT(jump_if_false, 0u);

    std::size_t loop_end = operand_at(compiled, jump_if_false);
    bool has_back_edge = false;
    bool has_break_jump = false;
    for (std::size_t i = 0; i < ops.size(); ++i) {
        if (ops[i] != fiber::script::ir::Code::JUMP) {
            continue;
        }
        std::size_t target = operand_at(compiled, i);
        if (target == iterate_next) {
            has_back_edge = true;
        }
        if (target == loop_end) {
            has_break_jump = true;
        }
    }

    EXPECT_TRUE(has_back_edge);
    EXPECT_TRUE(has_break_jump);
    EXPECT_LE(loop_end, compiled.code_size());
}

TEST(ScriptCompilerTest, ReportsTooManyDirectCallArguments) {
    CompileErrorLibrary library;
    std::string script = "variadic(";
    for (std::size_t i = 0; i < 256; ++i) {
        if (i != 0) {
            script.push_back(',');
        }
        script.push_back('0');
    }
    script.append(");");

    fiber::script::parse::Parser parser(library, true);
    auto parsed = parser.parse_script(script);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;

    auto compiled = fiber::script::ir::Compiler::compile(*parsed.value());
    ASSERT_FALSE(compiled.has_value());
    EXPECT_EQ(compiled.error().reason, fiber::script::ir::CompileErrorReason::TooManyArguments);
}

TEST(ScriptCompilerTest, ReportsUnsupportedDefaultConstantType) {
    CompileErrorLibrary library;
    fiber::script::parse::Parser parser(library, true);
    auto parsed = parser.parse_script("defaultException();");
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;

    auto compiled = fiber::script::ir::Compiler::compile(*parsed.value());
    ASSERT_FALSE(compiled.has_value());
    EXPECT_EQ(compiled.error().reason, fiber::script::ir::CompileErrorReason::UnsupportedConstant);
}
