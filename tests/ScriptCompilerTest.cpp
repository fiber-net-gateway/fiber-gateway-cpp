#include <gtest/gtest.h>

#include <cstddef>
#include <string_view>
#include <vector>

#include "script/Library.h"
#include "script/ir/Code.h"
#include "script/ir/Compiler.h"
#include "script/parse/Parser.h"

namespace {

class TestLibrary final : public fiber::script::Library {
public:
    Function *find_func(std::string_view name) override {
        (void)name;
        return nullptr;
    }

    AsyncFunction *find_async_func(std::string_view name) override {
        (void)name;
        return nullptr;
    }

    Constant *find_constant(std::string_view namespace_name, std::string_view key) override {
        (void)namespace_name;
        (void)key;
        return nullptr;
    }

    AsyncConstant *find_async_constant(std::string_view namespace_name, std::string_view key) override {
        (void)namespace_name;
        (void)key;
        return nullptr;
    }

    DirectiveDef *find_directive_def(std::string_view type,
                                     std::string_view name,
                                     const std::vector<fiber::json::JsValue> &literals) override {
        (void)type;
        (void)name;
        (void)literals;
        return nullptr;
    }
};

fiber::script::ir::Compiled compile_script(std::string_view script) {
    TestLibrary library;
    fiber::script::parse::Parser parser(library, true);
    auto parsed = parser.parse_script(script);
    EXPECT_TRUE(parsed.has_value()) << parsed.error().message;
    return fiber::script::ir::Compiler::compile(*parsed.value());
}

std::vector<std::uint8_t> extract_opcodes(const fiber::script::ir::Compiled &compiled) {
    std::vector<std::uint8_t> ops;
    ops.reserve(compiled.codes.size());
    for (std::int32_t code : compiled.codes) {
        ops.push_back(static_cast<std::uint8_t>(code & 0xFF));
    }
    return ops;
}

std::size_t operand_at(const fiber::script::ir::Compiled &compiled, std::size_t index) {
    return static_cast<std::size_t>(compiled.codes[index] >> 8);
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
    EXPECT_LE(end_target, compiled.codes.size());
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
    EXPECT_LE(loop_end, compiled.codes.size());
}
