#include <gtest/gtest.h>

#include <string_view>

#include "script/Library.h"
#include "script/parse/Parser.h"
#include "script/ast/BinaryOperator.h"
#include "script/ast/Block.h"
#include "script/ast/ExpressionStatement.h"
#include "script/ast/IfStatement.h"
#include "script/ast/InlineList.h"
#include "script/ast/InlineObject.h"
#include "script/ast/Literal.h"
#include "script/ast/Operator.h"
#include "script/ast/ReturnStatement.h"
#include "script/ast/UnaryOperator.h"
#include "script/ast/VariableReference.h"

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

const fiber::script::ast::Literal *as_literal(const fiber::script::ast::Expression *expr) {
    return dynamic_cast<const fiber::script::ast::Literal *>(expr);
}

const fiber::script::ast::VariableReference *as_variable(const fiber::script::ast::Expression *expr) {
    return dynamic_cast<const fiber::script::ast::VariableReference *>(expr);
}

const fiber::script::ast::BinaryOperator *as_binary(const fiber::script::ast::Expression *expr) {
    return dynamic_cast<const fiber::script::ast::BinaryOperator *>(expr);
}

const fiber::script::ast::UnaryOperator *as_unary(const fiber::script::ast::Expression *expr) {
    return dynamic_cast<const fiber::script::ast::UnaryOperator *>(expr);
}

const fiber::script::ast::InlineList *as_inline_list(const fiber::script::ast::Expression *expr) {
    return dynamic_cast<const fiber::script::ast::InlineList *>(expr);
}

const fiber::script::ast::InlineObject *as_inline_object(const fiber::script::ast::Expression *expr) {
    return dynamic_cast<const fiber::script::ast::InlineObject *>(expr);
}

} // namespace

TEST(ScriptParserTest, ParseIntegerLiteral) {
    TestLibrary library;
    fiber::script::parse::Parser parser(library, true);

    auto result = parser.parse_expression("42");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    const auto *literal = as_literal(result.value().get());
    ASSERT_NE(literal, nullptr);
    EXPECT_EQ(literal->kind(), fiber::script::ast::Literal::Kind::Integer);
    EXPECT_EQ(literal->int_value(), 42);
}

TEST(ScriptParserTest, ParseStringLiteral) {
    TestLibrary library;
    fiber::script::parse::Parser parser(library, true);

    auto result = parser.parse_expression("\"hi\"");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    const auto *literal = as_literal(result.value().get());
    ASSERT_NE(literal, nullptr);
    EXPECT_EQ(literal->kind(), fiber::script::ast::Literal::Kind::String);
    EXPECT_EQ(literal->string_value(), "hi");
}

TEST(ScriptParserTest, ParseIdentifier) {
    TestLibrary library;
    fiber::script::parse::Parser parser(library, true);

    auto result = parser.parse_expression("foo");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    const auto *identifier = as_variable(result.value().get());
    ASSERT_NE(identifier, nullptr);
    EXPECT_EQ(identifier->name(), "foo");
}

TEST(ScriptParserTest, ParseBinaryPrecedence) {
    TestLibrary library;
    fiber::script::parse::Parser parser(library, true);

    auto result = parser.parse_expression("1 + 2 * 3");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    const auto *root = as_binary(result.value().get());
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->op(), fiber::script::ast::Operator::Add);

    const auto *left = as_literal(root->left());
    ASSERT_NE(left, nullptr);
    EXPECT_EQ(left->int_value(), 1);

    const auto *right = as_binary(root->right());
    ASSERT_NE(right, nullptr);
    EXPECT_EQ(right->op(), fiber::script::ast::Operator::Multiply);

    const auto *right_left = as_literal(right->left());
    const auto *right_right = as_literal(right->right());
    ASSERT_NE(right_left, nullptr);
    ASSERT_NE(right_right, nullptr);
    EXPECT_EQ(right_left->int_value(), 2);
    EXPECT_EQ(right_right->int_value(), 3);
}

TEST(ScriptParserTest, ParseBinaryWithParentheses) {
    TestLibrary library;
    fiber::script::parse::Parser parser(library, true);

    auto result = parser.parse_expression("(1 + 2) * 3");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    const auto *root = as_binary(result.value().get());
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->op(), fiber::script::ast::Operator::Multiply);

    const auto *left = as_binary(root->left());
    ASSERT_NE(left, nullptr);
    EXPECT_EQ(left->op(), fiber::script::ast::Operator::Add);

    const auto *left_left = as_literal(left->left());
    const auto *left_right = as_literal(left->right());
    ASSERT_NE(left_left, nullptr);
    ASSERT_NE(left_right, nullptr);
    EXPECT_EQ(left_left->int_value(), 1);
    EXPECT_EQ(left_right->int_value(), 2);

    const auto *right = as_literal(root->right());
    ASSERT_NE(right, nullptr);
    EXPECT_EQ(right->int_value(), 3);
}

TEST(ScriptParserTest, ParseInlineList) {
    TestLibrary library;
    fiber::script::parse::Parser parser(library, true);

    auto result = parser.parse_expression("[1, 2, 3]");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    const auto *list = as_inline_list(result.value().get());
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->values().size(), 3u);
    const auto *lit0 = as_literal(list->values()[0].get());
    const auto *lit1 = as_literal(list->values()[1].get());
    const auto *lit2 = as_literal(list->values()[2].get());
    ASSERT_NE(lit0, nullptr);
    ASSERT_NE(lit1, nullptr);
    ASSERT_NE(lit2, nullptr);
    EXPECT_EQ(lit0->int_value(), 1);
    EXPECT_EQ(lit1->int_value(), 2);
    EXPECT_EQ(lit2->int_value(), 3);
}

TEST(ScriptParserTest, ParseInlineObject) {
    TestLibrary library;
    fiber::script::parse::Parser parser(library, true);

    auto result = parser.parse_expression("{a: 1, b: 2}");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    const auto *obj = as_inline_object(result.value().get());
    ASSERT_NE(obj, nullptr);
    ASSERT_EQ(obj->entries().size(), 2u);
    ASSERT_EQ(obj->entries()[0].key.kind, fiber::script::ast::InlineObject::KeyKind::String);
    ASSERT_EQ(obj->entries()[1].key.kind, fiber::script::ast::InlineObject::KeyKind::String);
    EXPECT_EQ(obj->entries()[0].key.string_key, "a");
    EXPECT_EQ(obj->entries()[1].key.string_key, "b");
    const auto *val0 = as_literal(obj->entries()[0].value.get());
    const auto *val1 = as_literal(obj->entries()[1].value.get());
    ASSERT_NE(val0, nullptr);
    ASSERT_NE(val1, nullptr);
    EXPECT_EQ(val0->int_value(), 1);
    EXPECT_EQ(val1->int_value(), 2);
}

TEST(ScriptParserTest, ParseScriptExpressionStatement) {
    TestLibrary library;
    fiber::script::parse::Parser parser(library, true);

    auto result = parser.parse_script("1 + 2;");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    const auto &statements = result.value()->statements();
    ASSERT_EQ(statements.size(), 1u);
    const auto *stmt = dynamic_cast<const fiber::script::ast::ExpressionStatement *>(statements.front().get());
    ASSERT_NE(stmt, nullptr);
    EXPECT_NE(as_binary(stmt->expression()), nullptr);
}

TEST(ScriptParserTest, ParseUnaryOps) {
    TestLibrary library;
    fiber::script::parse::Parser parser(library, true);

    auto result = parser.parse_expression("-!x");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    const auto *outer = as_unary(result.value().get());
    ASSERT_NE(outer, nullptr);
    EXPECT_EQ(outer->op(), fiber::script::ast::Operator::Minus);

    const auto *inner = as_unary(outer->operand());
    ASSERT_NE(inner, nullptr);
    EXPECT_EQ(inner->op(), fiber::script::ast::Operator::Not);
    EXPECT_NE(as_variable(inner->operand()), nullptr);
}

TEST(ScriptParserTest, ParseReturnStatement) {
    TestLibrary library;
    fiber::script::parse::Parser parser(library, true);

    auto result = parser.parse_script("return 1 + 2;");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    const auto &statements = result.value()->statements();
    ASSERT_EQ(statements.size(), 1u);
    const auto *stmt = dynamic_cast<const fiber::script::ast::ReturnStatement *>(statements.front().get());
    ASSERT_NE(stmt, nullptr);
    EXPECT_NE(as_binary(stmt->value()), nullptr);
}

TEST(ScriptParserTest, ParseIfStatement) {
    TestLibrary library;
    fiber::script::parse::Parser parser(library, true);

    auto result = parser.parse_script("if (a) { return 1; } else { return 2; }");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    const auto &statements = result.value()->statements();
    ASSERT_EQ(statements.size(), 1u);
    const auto *stmt = dynamic_cast<const fiber::script::ast::IfStatement *>(statements.front().get());
    ASSERT_NE(stmt, nullptr);
    EXPECT_NE(as_variable(stmt->condition()), nullptr);
    const auto *then_block = dynamic_cast<const fiber::script::ast::Block *>(stmt->then_branch());
    ASSERT_NE(then_block, nullptr);
    ASSERT_EQ(then_block->statements().size(), 1u);
    EXPECT_NE(dynamic_cast<const fiber::script::ast::ReturnStatement *>(then_block->statements().front().get()), nullptr);

    const auto *else_block = dynamic_cast<const fiber::script::ast::Block *>(stmt->else_branch());
    ASSERT_NE(else_block, nullptr);
    ASSERT_EQ(else_block->statements().size(), 1u);
    EXPECT_NE(dynamic_cast<const fiber::script::ast::ReturnStatement *>(else_block->statements().front().get()), nullptr);
}

TEST(ScriptParserTest, ParseTypeofUnary) {
    TestLibrary library;
    fiber::script::parse::Parser parser(library, true);

    auto result = parser.parse_expression("typeof foo");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    const auto *expr = as_unary(result.value().get());
    ASSERT_NE(expr, nullptr);
    EXPECT_EQ(expr->op(), fiber::script::ast::Operator::Typeof);
    EXPECT_NE(as_variable(expr->operand()), nullptr);
}

TEST(ScriptParserTest, ParseReturnWithoutValue) {
    TestLibrary library;
    fiber::script::parse::Parser parser(library, true);

    auto result = parser.parse_script("return;");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    const auto &statements = result.value()->statements();
    ASSERT_EQ(statements.size(), 1u);
    const auto *stmt = dynamic_cast<const fiber::script::ast::ReturnStatement *>(statements.front().get());
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(stmt->value(), nullptr);
}

TEST(ScriptParserTest, ParseBlockStatement) {
    TestLibrary library;
    fiber::script::parse::Parser parser(library, true);

    auto result = parser.parse_script("{ return 1; return 2; }");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    const auto &statements = result.value()->statements();
    ASSERT_EQ(statements.size(), 1u);
    const auto *block = dynamic_cast<const fiber::script::ast::Block *>(statements.front().get());
    ASSERT_NE(block, nullptr);
    EXPECT_EQ(block->statements().size(), 2u);
}

TEST(ScriptParserTest, ParseInvalidExpression) {
    TestLibrary library;
    fiber::script::parse::Parser parser(library, true);

    auto result = parser.parse_expression("1 +");
    ASSERT_FALSE(result.has_value());
    EXPECT_FALSE(result.error().message.empty());
}
